/*
    This file is part of tgl-library

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

    Copyright Vitaly Valtman 2013-2015
    Copyright Topology LP 2016-2017
*/

#include "query_messages_send_encrypted_file.h"

#include "auto/constants.h"
#include "auto/auto_fetch_ds.h"
#include "auto/auto_free_ds.h"
#include "auto/auto_skip.h"
#include "crypto/crypto_md5.h"
#include "document.h"
#include "message.h"
#include "secret_chat_encryptor.h"
#include "tgl/tgl_mime_type.h"
#include "transfer_manager.h"
#include "upload_task.h"

#include <boost/filesystem.hpp>
#include <cstring>

namespace tgl {
namespace impl {

struct query_messages_send_encrypted_file::decrypted_message_media {
    explicit decrypted_message_media(tgl_in_buffer in)
        : decrypted_message_media_type(TYPE_TO_PARAM(decrypted_message_media))
    {
        tgl_in_buffer skip_in = in;
        auto result = skip_type_any(&skip_in, &decrypted_message_media_type);
        TGL_ASSERT_UNUSED(result, result >= 0);
        assert(skip_in.ptr == skip_in.end);
        media = fetch_ds_type_decrypted_message_media(&in, &decrypted_message_media_type);
        assert(media);
        assert(in.ptr == in.end);
    }

    ~decrypted_message_media()
    {
        if (media) {
            free_ds_type_decrypted_message_media(media, &decrypted_message_media_type);
        }
    }

    paramed_type decrypted_message_media_type;
    tl_ds_decrypted_message_media* media;
};

query_messages_send_encrypted_file::query_messages_send_encrypted_file(
        user_agent& ua,
        const std::shared_ptr<secret_chat>& sc,
        const std::shared_ptr<upload_task>& upload,
        const std::shared_ptr<message>& m,
        const std::function<void(bool, const std::shared_ptr<message>&)>& callback)
    : query_messages_send_encrypted_base(ua, "send encrypted file message", sc, m, callback, false)
    , m_upload(upload)
{
}

query_messages_send_encrypted_file::query_messages_send_encrypted_file(
        user_agent& ua,
        const std::shared_ptr<secret_chat>& sc,
        const std::shared_ptr<tgl_unconfirmed_secret_message>& unconfirmed_message,
        const std::function<void(bool, const std::shared_ptr<message>&)>& callback) throw(std::runtime_error)
    : query_messages_send_encrypted_base(ua, "send encrypted file message (reassembled)", sc, nullptr, callback, true)
{
    if (sc->layer() < 17) {
        throw std::runtime_error("we shouldn't have tried to construct a query from unconfirmed message "
                "for the secret chat with layer less than 17");
    }

    const auto& blobs = unconfirmed_message->blobs();
    if (unconfirmed_message->constructor_code() != CODE_messages_send_encrypted_file
            || blobs.size() != 2) {
        throw std::runtime_error("invalid message blobs for query_messages_send_encrypted_file");
    }

    const std::string& layer_blob = blobs[0];
    const std::string& input_file_info_blob = blobs[1];
    if ((layer_blob.size() % 4) || (input_file_info_blob.size() % 4)) {
        throw std::runtime_error("message blobs for query_messages_send_encrypted_file don't align in 4 bytes boundary");
    }

    out_i32(CODE_messages_send_encrypted_file);
    out_i32(CODE_input_encrypted_chat);
    out_i32(m_secret_chat->id().peer_id);
    out_i64(m_secret_chat->id().access_hash);
    out_i64(unconfirmed_message->message_id());
    secret_chat_encryptor encryptor(m_secret_chat->key_fingerprint(), m_secret_chat->encryption_key(), serializer());
    encryptor.start();
    out_i32s(reinterpret_cast<const int32_t*>(layer_blob.data()), layer_blob.size() / 4);
    encryptor.end();
    out_i32s(reinterpret_cast<const int32_t*>(input_file_info_blob.data()), input_file_info_blob.size() / 4);

    construct_message(unconfirmed_message->message_id(), unconfirmed_message->date(), layer_blob);
    auto media = m_message->media();
    m_message->set_media(std::make_shared<tgl_message_media_none>());
    m_user_agent.callback()->update_messages({m_message});
    m_message->set_media(media);
}

query_messages_send_encrypted_file::~query_messages_send_encrypted_file()
{
}

void query_messages_send_encrypted_file::set_message_media(const tl_ds_decrypted_message_media* DS_DMM)
{
    m_message->set_decrypted_message_media(DS_DMM);

    if (m_message->media()->type() == tgl_message_media_type::document) {
        auto media = std::static_pointer_cast<tgl_message_media_document>(m_message->media());
        auto document = std::static_pointer_cast<class document>(media->document);
        if (document && document->is_encrypted()) {
            const auto& u = m_upload;
            if (u->is_image() || u->as_photo) {
                document->set_type(tgl_document_type::image);
                if (u->is_animated()) {
                    document->set_animated(true);
                } else {
                    document->set_animated(false);
                }
            } else if (u->is_video()) {
                document->set_type(tgl_document_type::video);
            } else if (u->is_audio()) {
                document->set_type(tgl_document_type::audio);
            } else if (u->is_sticker()) {
                document->set_type(tgl_document_type::sticker);
            } else {
                document->set_type(tgl_document_type::unknown);
            }
        }
    }
}

void query_messages_send_encrypted_file::assemble()
{
    const auto& u = m_upload;
    int32_t layer = m_secret_chat->layer();

    out_i32(CODE_messages_send_encrypted_file);
    out_i32(CODE_input_encrypted_chat);
    out_i32(u->to_id.peer_id);
    out_i64(m_secret_chat->id().access_hash);
    out_i64(m_message->id());
    secret_chat_encryptor encryptor(m_secret_chat->key_fingerprint(), m_secret_chat->encryption_key(), serializer());
    encryptor.start();
    size_t capture_start = 0;

    if (layer >= 17) {
        capture_start = begin_unconfirmed_message(CODE_messages_send_encrypted_file);
        out_i32(CODE_decrypted_message_layer);
        out_random(15 + 4 * (tgl_random<int>() % 3));
        out_i32(m_secret_chat->layer());
        out_i32(m_secret_chat->raw_in_seq_no());
        out_i32(m_secret_chat->raw_out_seq_no());
    }

    if (layer >= 46) {
        out_i32(CODE_decrypted_message);
        out_i32(1 << 9);
        out_i64(m_message->id());
        out_i32(m_secret_chat->ttl());
    } else if (layer >= 17) {
        out_i32(CODE_decrypted_message_layer17);
        out_i64(m_message->id());
        out_i32(m_secret_chat->ttl());
    } else {
        out_i32(CODE_decrypted_message_layer8);
        out_i64(m_message->id());
        out_random(15 + 4 * (tgl_random<int>() % 3));
        if (layer < 8) {
            TGL_ERROR("invalid secret chat layer " << layer);
            assert(false);
        }
    }

    out_string("");

    size_t start = serializer()->i32_size();

    if (u->as_photo) {
        if (layer >= 17) {
            out_i32(CODE_decrypted_message_media_photo);
        } else {
            out_i32(CODE_decrypted_message_media_photo_layer8);
        }
    } else if (u->is_video()) {
        if (layer >= 46) {
            out_i32(CODE_decrypted_message_media_video);
        } else if (layer >= 17) {
            out_i32(CODE_decrypted_message_media_video_layer17);
        } else {
            out_i32(CODE_decrypted_message_media_video_layer8);
        }
    } else if (u->is_audio()) {
        if (layer >= 17) {
            out_i32(CODE_decrypted_message_media_audio);
        } else {
            out_i32(CODE_decrypted_message_media_audio_layer8);
        }
    } else {
        if (layer >= 46 ) {
            out_i32(CODE_decrypted_message_media_document);
        } else {
            out_i32(CODE_decrypted_message_media_document_layer8);
        }
    }
    if (u->as_photo || !u->is_audio()) {
        TGL_DEBUG("secret chat thumb data " << u->thumb.size() << " bytes @ " << u->thumb_width << "x" << u->thumb_height);
        out_string(reinterpret_cast<const char*>(u->thumb.data()), u->thumb.size());
        out_i32(u->thumb_width);
        out_i32(u->thumb_height);
    }

    bool is_document = false;
    if (u->as_photo) {
        out_i32(u->width);
        out_i32(u->height);
    } else if (u->is_video()) {
        out_i32(u->duration);
        if (layer >= 17) {
            out_std_string(tgl_mime_type_by_filename(u->file_name));
        }
        out_i32(u->width);
        out_i32(u->height);
    } else if (u->is_audio()) {
        out_i32(u->duration);
        if (layer >= 17) {
            out_std_string(tgl_mime_type_by_filename(u->file_name));
        }
    } else {
        is_document = true;
        if (layer >= 46) {
            out_std_string(tgl_mime_type_by_filename(u->file_name));
        } else {
            boost::filesystem::path path(u->file_name);
            out_std_string(path.filename().string());
            out_std_string(tgl_mime_type_by_filename(u->file_name));
        }
    }

    out_i32(u->size);
    out_string(reinterpret_cast<const char*>(u->key.data()), u->key.size());
    out_string(reinterpret_cast<const char*>(u->init_iv.data()), u->init_iv.size());

    if (layer >= 46) {
        if (u->is_video()) {
            out_string(""); // caption
        } else if (is_document) {
            boost::filesystem::path path(u->file_name);
            out_i32(CODE_vector);
            out_i32(1);
            out_i32(CODE_document_attribute_filename);
            out_std_string(path.filename().string());
            out_string(""); // caption
        }
    }

    if (layer >= 17) {
        if (u->as_photo) {
            out_string(""); // caption
        }
    }

    tgl_in_buffer in = { serializer()->i32_data() + start, serializer()->i32_data() + serializer()->i32_size() };
    m_decrypted_message_media = std::make_unique<decrypted_message_media>(in);

    if (layer >= 17) {
        append_blob_to_unconfirmed_message(capture_start);
    }

    encryptor.end();

    capture_start = serializer()->char_size();
    if (u->size < BIG_FILE_THRESHOLD) {
        out_i32(CODE_input_encrypted_file_uploaded);
    } else {
        out_i32(CODE_input_encrypted_file_big_uploaded);
    }
    out_i64(u->id);
    out_i32(u->part_num);
    if (u->size < BIG_FILE_THRESHOLD) {
        out_string("");
    }

    unsigned char md5[16];
    unsigned char str[64];
    memcpy(str, u->key.data(), 32);
    memcpy(str + 32, u->init_iv.data(), 32);
    TGLC_md5(str, 64, md5);
    int32_t key_fingerprint = (*(int32_t *)md5) ^ (*(int32_t *)(md5 + 4));
    out_i32(key_fingerprint);

    if (layer >= 17) {
        append_blob_to_unconfirmed_message(capture_start);
    }
}

void query_messages_send_encrypted_file::on_answer(void* D)
{
    if (m_decrypted_message_media) {
        set_message_media(m_decrypted_message_media->media);
        m_decrypted_message_media = nullptr;
    }

    query_messages_send_encrypted_base::on_answer(D);
}

}
}
