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
    Copyright Topology LP 2016
*/

#include "queries.h"

#include <algorithm>
#include <errno.h>
#include <fcntl.h>
#include <memory.h>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifndef WIN32
#include <sys/utsname.h>
#endif

#include "auto/auto.h"
#include "auto/auto-fetch-ds.h"
#include "auto/auto-free-ds.h"
#include "auto/auto-skip.h"
#include "auto/auto-types.h"
#include "crypto/tgl_crypto_md5.h"
#include "crypto/tgl_crypto_rand.h"
#include "crypto/tgl_crypto_sha.h"
#include "mtproto-client.h"
#include "mtproto-common.h"
#include "mtproto-utils.h"
#include "queries-encrypted.h"
#include "structures.h"
#include "tools.h"
#include "tgl/tgl.h"
#include "tgl/tgl_chat.h"
#include "tgl/tgl_log.h"
#include "tgl/tgl_peer_id.h"
#include "tgl/tgl_privacy_rule.h"
#include "tgl/tgl_queries.h"
#include "tgl/tgl_secure_random.h"
#include "tgl/tgl_timer.h"
#include "tgl/tgl_update_callback.h"
#include "updates.h"

constexpr int32_t TGL_SCHEME_LAYER = 45;
constexpr int TGL_MAX_DC_NUM = 100;
const char* TGL_VERSION = "0.1.0";

void query::clear_timers()
{
    if (m_timer) {
        m_timer->cancel();
    }
    m_timer = nullptr;

    if (m_retry_timer) {
        m_retry_timer->cancel();
    }
    m_retry_timer = nullptr;
}

void query::alarm()
{
    TGL_DEBUG("alarm query #" << msg_id() << " (type '" << m_name << "') to DC " << m_dc->id);
    clear_timers();

    if (!check_connectivity()) {
        return;
    }

    if (!check_logging_out()) {
        return;
    }

    bool pending = false;
    if (!m_dc->is_configured() && !is_force()) {
        pending = true;
    }

    if (!m_dc->is_logged_in() && !is_login() && !is_force()) {
        pending = true;
    }

    if (!pending && m_session && m_session_id && m_dc && m_dc->session == m_session && m_session->session_id == m_session_id) {
        mtprotocol_serializer s;
        s.out_i32(CODE_msg_container);
        s.out_i32(1);
        s.out_i64(msg_id());
        s.out_i32(m_seq_no);
        s.out_i32(m_serializer->char_size());
        s.out_i32s(m_serializer->i32_data(), m_serializer->i32_size());
        if (tglmp_encrypt_send_message(m_session->c, s.i32_data(), s.i32_size(), m_msg_id_override, is_force()) == -1) {
            handle_error(400, "client failed to send message");
            return;
        }
        TGL_NOTICE("resent query #" << msg_id() << " of size " << m_serializer->char_size() << " to DC " << m_dc->id);
        timeout_within(timeout_interval());
    } else if (!pending && m_dc->session) {
        m_ack_received = false;
        if (msg_id()) {
            tgl_state::instance()->remove_query(shared_from_this());
        }
        m_session = m_dc->session;
        int64_t old_id = msg_id();
        m_msg_id = tglmp_encrypt_send_message(m_session->c, m_serializer->i32_data(), m_serializer->i32_size(), m_msg_id_override, is_force(), true);
        if (m_msg_id == -1) {
            m_msg_id = 0;
            handle_error(400, "client failed to send message");
            return;
        }
        TGL_NOTICE("resent query #" << old_id << " as #" << msg_id() << " of size " << m_serializer->char_size() << " to DC " << m_dc->id);
        tgl_state::instance()->add_query(shared_from_this());
        m_session_id = m_session->session_id;
        auto dc = m_session->dc.lock();
        if (dc && !dc->is_configured() && !is_force()) {
            m_session_id = 0;
        }
        timeout_within(timeout_interval());
    } else {
        will_be_pending();
        // we don't have a valid session with the DC, so defer query until we do
        m_dc->add_pending_query(shared_from_this());
        TGL_DEBUG("added query #" << msg_id() << "(type '" << name() << "') to pending list");
    }
}

void tglq_regen_query(int64_t id)
{
    std::shared_ptr<query> q = tgl_state::instance()->get_query(id);
    if (!q) {
        return;
    }
    TGL_NOTICE("regen query " << id);
    q->regen();
}

void query::regen()
{
    m_ack_received = false;
    if (!(m_session && m_session_id && m_dc && m_dc->session == m_session && m_session->session_id == m_session_id)) {
        m_session_id = 0;
    } else {
        auto dc = m_session->dc.lock();
        if (dc && !dc->is_configured() && !is_force()) {
            m_session_id = 0;
        }
    }
    retry_within(0);
}

void tglq_query_restart(int64_t id)
{
    std::shared_ptr<query> q = tgl_state::instance()->get_query(id);
    if (q) {
        TGL_NOTICE("restarting query " << id);
        q->alarm();
    }
}

void query::timeout_alarm()
{
    clear_timers();
    on_timeout();
    if (!should_retry_on_timeout()) {
        if (msg_id()) {
            tgl_state::instance()->remove_query(shared_from_this());
        }
        m_dc->remove_pending_query(shared_from_this());
    } else {
        alarm();
    }
}

static void tgl_transfer_auth_callback(const std::shared_ptr<tgl_dc>& arg, bool success);
static void tgl_do_transfer_auth(const std::shared_ptr<tgl_dc>& dc, const std::function<void(bool success)>& callback);

void query::execute(const std::shared_ptr<tgl_dc>& dc, execution_option option)
{
    if (!check_connectivity()) {
        return;
    }

    m_ack_received = false;
    m_exec_option = option;
    m_dc = dc;
    assert(m_dc);

    if (!check_logging_out()) {
        return;
    }

    bool pending = false;
    if (!m_dc->session) {
        tglmp_dc_create_session(m_dc);
        pending = true;
    }

    if (!m_dc->is_configured() && !is_force()) {
        pending = true;
    }

    if (!m_dc->is_logged_in() && !is_login() && !is_force()) {
        pending = true;
        if (m_dc != tgl_state::instance()->working_dc()) {
            tgl_do_transfer_auth(m_dc, std::bind(tgl_transfer_auth_callback, m_dc, std::placeholders::_1));
        }
    }

    TGL_DEBUG("sending query \"" << m_name << "\" of size " << m_serializer->char_size() << " to DC " << m_dc->id << (pending ? " (pending)" : ""));

    if (pending) {
        will_be_pending();
        m_msg_id = 0;
        m_session = 0;
        m_session_id = 0;
        m_seq_no = 0;
        m_dc->add_pending_query(shared_from_this());
    } else {
        m_msg_id = tglmp_encrypt_send_message(m_dc->session->c, m_serializer->i32_data(), m_serializer->i32_size(), m_msg_id_override, is_force(), true);
        if (m_msg_id == -1) {
            m_msg_id = 0;
            handle_error(400, "client failed to send message");
            return;
        }

        if (is_logout()) {
            m_dc->set_logout_query_id(msg_id());
        }

        m_session = m_dc->session;
        m_session_id = m_session->session_id;
        m_seq_no = m_session->seq_no - 1;

        tgl_state::instance()->add_query(shared_from_this());
        timeout_within(timeout_interval());

        TGL_DEBUG("sent query \"" << m_name << "\" of size " << m_serializer->char_size() << " to DC " << m_dc->id << ": #" << msg_id());
    }
}

bool query::execute_after_pending()
{
    if (!check_connectivity()) {
        // We gave an error in check_connectity above. So this has been executed but failed.
        return true;
    }

    if (!check_logging_out()) {
        // We gave an error in check_logging_out above. So this has been executed but failed.
        return true;
    }

    assert(m_dc);
    assert(m_exec_option != execution_option::UNKNOWN);

    if (!m_dc->session) {
        tglmp_dc_create_session(m_dc);
    }

    bool pending = false;
    if (!m_dc->is_configured() && !is_force()) {
        pending = true;
    }

    if (!m_dc->is_logged_in() && !is_login() && !is_force()) {
        pending = true;
    }

    if (pending) {
        will_be_pending();
        TGL_DEBUG("not ready to send pending query " << this << " (" << m_name << "), re-queuing");
        m_dc->add_pending_query(shared_from_this());
        return false;
    }

    m_msg_id = tglmp_encrypt_send_message(m_dc->session->c, m_serializer->i32_data(), m_serializer->i32_size(), m_msg_id_override, is_force(), true);
    if (m_msg_id == -1) {
        m_msg_id = 0;
        handle_error(400, "client failed to send message");
        return true;
    }

    if (is_logout()) {
        m_dc->set_logout_query_id(msg_id());
    }

    m_ack_received = false;
    m_session = m_dc->session;
    tgl_state::instance()->add_query(shared_from_this());
    m_session_id = m_session->session_id;
    auto dc = m_session->dc.lock();
    if (dc && !dc->is_configured() && !is_force()) {
        m_session_id = 0;
    }

    TGL_DEBUG("sent pending query \"" << m_name << "\" (" << msg_id() << ") of size " << m_serializer->char_size() << " to DC " << m_dc->id);

    timeout_within(timeout_interval());

    return true;
}

void query::out_peer_id(const tgl_peer_id_t& id, int64_t access_hash)
{
    switch (id.peer_type) {
    case tgl_peer_type::chat:
        m_serializer->out_i32(CODE_input_peer_chat);
        m_serializer->out_i32(id.peer_id);
        break;
    case tgl_peer_type::user:
        if (id.peer_id == tgl_state::instance()->our_id().peer_id) {
            m_serializer->out_i32(CODE_input_peer_self);
        } else {
            m_serializer->out_i32(CODE_input_peer_user);
            m_serializer->out_i32(id.peer_id);
            m_serializer->out_i64(access_hash);
        }
        break;
    case tgl_peer_type::channel:
        m_serializer->out_i32(CODE_input_peer_channel);
        m_serializer->out_i32(id.peer_id);
        m_serializer->out_i64(access_hash);
        break;
    default:
        assert(false);
    }
}

void query::out_input_peer(const tgl_input_peer_t& id)
{
    out_peer_id(tgl_peer_id_t(id.peer_type, id.peer_id), id.access_hash);
}

void tglq_query_ack(int64_t id)
{
    std::shared_ptr<query> q = tgl_state::instance()->get_query(id);
    if (q) {
        q->ack();
    }
}

void query::ack()
{
    if (m_ack_received) {
        return;
    }

    m_ack_received = true;
    timeout_within(timeout_interval());

    // FIXME: This a workaround to the weird server behavour. The server
    // replies a logout query with ack and then closes the connection.
    if (is_logout()) {
        mtprotocol_serializer s;
        s.out_i32(CODE_bool_true);
        tgl_in_buffer in = { s.i32_data(), s.i32_data() + s.i32_size() };
        handle_result(&in);
    }
}

void tglq_query_delete(int64_t id)
{
    std::shared_ptr<query> q = tgl_state::instance()->get_query(id);
    if (!q) {
        return;
    }

    q->clear_timers();
    if (id) {
        tgl_state::instance()->remove_query(q);
    }
}

static void resend_query_cb(const std::shared_ptr<query>& q, bool success);

int tglq_query_error(tgl_in_buffer* in, int64_t id)
{
    int32_t result = fetch_i32(in);
    TGL_ASSERT_UNUSED(result, result == static_cast<int32_t>(CODE_rpc_error));
    int32_t error_code = fetch_i32(in);
    int error_len = prefetch_strlen(in);
    std::string error_string = std::string(fetch_str(in, error_len), error_len);
    std::shared_ptr<query> q = tgl_state::instance()->get_query(id);
    if (!q) {
        TGL_WARNING("error for unknown query #" << id << " #" << error_code << ": " << error_string);
    } else {
        TGL_WARNING("error for query '" << q->name() << "' #" << id << " #" << error_code << ": " << error_string);
        return q->handle_error(error_code, error_string);
    }

    return 0;
}

static void tgl_do_check_password(const std::function<void(bool success)>& callback);

static bool get_int_from_prefixed_string(int& number, const std::string& prefixed_string, const std::string& prefix)
{
    std::string number_string;
    if (prefixed_string.size() >= prefix.size() + 1 && !prefixed_string.compare(0, prefix.size(), prefix)) {
        number_string = prefixed_string.substr(prefix.size());
    }

    if (number_string.size()) {
        try {
            number = std::stoi(number_string);
            return true;
        } catch (...) {
            return false;
        }
    }

    return false;
}

static int get_dc_from_migration(const std::string& migration_error_string)
{
    int dc = -1;
    if (get_int_from_prefixed_string(dc, migration_error_string, "USER_MIGRATE_")) {
        return dc;
    }

    if (get_int_from_prefixed_string(dc, migration_error_string, "PHONE_MIGRATE_")) {
        return dc;
    }

    if (get_int_from_prefixed_string(dc, migration_error_string, "NETWORK_MIGRATE_")) {
        return dc;
    }

    return dc;
}

int query::handle_error(int error_code, const std::string& error_string)
{
    if (msg_id()) {
        tgl_state::instance()->remove_query(shared_from_this());
    }
    clear_timers();

    int retry_within_seconds = 0;
    bool should_retry = false;
    bool error_handled = false;

    switch (error_code) {
        case 303: // migrate
        {
            TGL_NOTICE("trying to handle migration error of " << error_string);
            int new_dc = get_dc_from_migration(error_string);
            if (new_dc > 0 && new_dc < TGL_MAX_DC_NUM) {
                tgl_state::instance()->set_working_dc(new_dc);
                auto dc = tgl_state::instance()->working_dc();

                if (!dc->is_authorized()) {
                    dc->restart_authorization();
                }

                m_ack_received = false;
                m_session_id = 0;
                m_dc = tgl_state::instance()->working_dc();
                if (should_retry_after_recover_from_error() || is_login()) {
                    should_retry = true;
                }
                error_handled = true;
            }
            break;
        }
        case 400:
            // nothing to handle
            // bad user input probably
            break;
        case 401:
            if (error_string == "SESSION_PASSWORD_NEEDED") {
                if (!tgl_state::instance()->is_password_locked()) {
                    tgl_state::instance()->set_password_locked(true);
                    tgl_do_check_password(std::bind(resend_query_cb, shared_from_this(), std::placeholders::_1));
                }
                if (should_retry_after_recover_from_error()) {
                    should_retry = true;
                }
                error_handled = true;
            } else if (error_string == "AUTH_KEY_UNREGISTERED" || error_string == "AUTH_KEY_INVALID") {
                tgl_do_set_dc_logged_out(m_dc, true);
                tgl_state::instance()->login();
                if (should_retry_after_recover_from_error()) {
                    should_retry = true;
                }
                error_handled = true;
            } else if (error_string == "AUTH_KEY_PERM_EMPTY") {
                assert(tgl_state::instance()->pfs_enabled());
                m_dc->restart_temp_authorization();
                if (should_retry_after_recover_from_error()) {
                    should_retry = true;
                }
                error_handled = true;
            }
            break;
        case 403: // privacy violation
            break;
        case 404: // not found
            break;
        case 420: // flood
        case 500: // internal error
        default: // anything else treated as internal error
        {
            if (!get_int_from_prefixed_string(retry_within_seconds, error_string, "FLOOD_WAIT_")) {
                if (error_code == 420) {
                    TGL_ERROR("error 420: " << error_string);
                }
                retry_within_seconds = 10;
            }
            m_ack_received = false;
            if (should_retry_after_recover_from_error()) {
                should_retry = true;
            }
            std::shared_ptr<tgl_dc> DC = m_dc;
            if (!DC->is_configured() && !is_force()) {
                m_session_id = 0;
            }
            error_handled = true;
            break;
        }
    }

    if (should_retry) {
        retry_within(retry_within_seconds);
    }

    if (error_handled) {
        TGL_NOTICE("error for query #" << msg_id() << " error:" << error_code << " " << error_string << " (HANDLED)");
        return 0;
    }

    return on_error(error_code, error_string);
}

void query::retry_within(double seconds)
{
    if (!m_retry_timer) {
        m_retry_timer = tgl_state::instance()->timer_factory()->create_timer(std::bind(&query::alarm, shared_from_this()));
    }
    m_retry_timer->start(seconds);
}

void query::timeout_within(double seconds)
{
    if (!m_timer) {
        m_timer = tgl_state::instance()->timer_factory()->create_timer(std::bind(&query::timeout_alarm, shared_from_this()));
    }

    m_timer->start(seconds);
}

bool query::check_connectivity()
{
    if (tgl_state::instance()->online_status() != tgl_online_status::not_online) {
        return true;
    }

    TGL_WARNING("we don't have internet connection, failing query (" << name() << ")");
    on_disconnected();
    return false;
}

bool query::check_logging_out()
{
    if (m_dc->is_logging_out()) {
        assert(!is_logout());
        if (!is_force()) {
            on_error(600, "LOGGING_OUT");
            return false;
        }
    }

    return true;
}

void query::on_disconnected()
{
    on_error(600, "NOT_CONNECTED");
}

int tglq_query_result(tgl_in_buffer* in, int64_t id)
{
    std::shared_ptr<query> q = tgl_state::instance()->get_query(id);
    if (!q) {
        in->ptr = in->end;
        return 0;
    }

    return q->handle_result(in);
}

int query::handle_result(tgl_in_buffer* in)
{
    int32_t op = prefetch_i32(in);

    tgl_in_buffer save_in = { nullptr, nullptr };
    std::unique_ptr<int32_t[]> packed_buffer;

    if (op == CODE_gzip_packed) {
        fetch_i32(in);
        int l = prefetch_strlen(in);
        const char* s = fetch_str(in, l);

        constexpr size_t MAX_PACKED_SIZE = 1 << 24;
        packed_buffer.reset(new int32_t[MAX_PACKED_SIZE / 4]);

        int total_out = tgl_inflate(s, l, packed_buffer.get(), MAX_PACKED_SIZE);
        TGL_DEBUG("inflated " << total_out << " bytes");
        save_in = *in;
        in->ptr = packed_buffer.get();
        in->end = in->ptr + total_out / 4;
    }

    TGL_DEBUG("result for query #" << msg_id() << ". Size " << (long)4 * (in->end - in->ptr) << " bytes");

    tgl_in_buffer skip_in = *in;
    if (skip_type_any(&skip_in, &m_type) < 0) {
        TGL_ERROR("skipped " << (long)(skip_in.ptr - in->ptr) << " int out of " << (long)(skip_in.end - in->ptr) << " (type " << m_type.type.id << ") (query type " << name() << ")");
        TGL_ERROR("0x" << std::hex << *(in->ptr - 1) << " 0x" << *(in->ptr) << " 0x" << *(in->ptr + 1) << " 0x" << *(in->ptr + 2));
        TGL_ERROR(in->print_buffer());
        assert(false);
    }

    assert(skip_in.ptr == skip_in.end);

    void* DS = fetch_ds_type_any(in, &m_type);
    assert(DS);

    on_answer(DS);
    free_ds_type_any(DS, &m_type);

    assert(in->ptr == in->end);

    clear_timers();
    tgl_state::instance()->remove_query(shared_from_this());

    if (save_in.ptr) {
        *in = save_in;
    }

    return 0;
}

void query::out_header()
{
    m_serializer->out_i32(CODE_invoke_with_layer);
    m_serializer->out_i32(TGL_SCHEME_LAYER);
    m_serializer->out_i32(CODE_init_connection);
    m_serializer->out_i32(tgl_state::instance()->app_id());

    m_serializer->out_string("x86");
    m_serializer->out_string("OSX");
    std::string buf = tgl_state::instance()->app_version() + " (TGL " + TGL_VERSION + ")";
    m_serializer->out_std_string(buf);
    m_serializer->out_string("en");
}

/* {{{ Get config */

void fetch_dc_option(const tl_ds_dc_option* DS_DO)
{
    if (DS_BOOL(DS_DO->media_only)) { // We do not support media only ip addresses yet
        return;
    }
    tgl_state::instance()->set_dc_option(DS_BOOL(DS_DO->ipv6),
            DS_LVAL(DS_DO->id),
            DS_STDSTR(DS_DO->ip_address),
            DS_LVAL(DS_DO->port));
}

class query_help_get_config: public query
{
public:
    explicit query_help_get_config(const std::function<void(bool)>& callback)
        : query("get config", TYPE_TO_PARAM(config))
        , m_callback(callback)
    { }

    virtual void on_answer(void* DS) override
    {
        tl_ds_config* DS_C = static_cast<tl_ds_config*>(DS);

        int count = DS_LVAL(DS_C->dc_options->cnt);
        for (int i = 0; i < count; ++i) {
            fetch_dc_option(DS_C->dc_options->data[i]);
        }

        int max_chat_size = DS_LVAL(DS_C->chat_size_max);
        int max_bcast_size = 0; //DS_LVAL(DS_C->broadcast_size_max);
        TGL_DEBUG("chat_size = " << max_chat_size << ", bcast_size = " << max_bcast_size);

        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

    virtual double timeout_interval() const override { return 1; }

private:
    std::function<void(bool)> m_callback;
};

void tgl_do_help_get_config(const std::function<void(bool)>& callback)
{
    auto q = std::make_shared<query_help_get_config>(callback);
    q->out_header();
    q->out_i32(CODE_help_get_config);
    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_help_get_config_dc(const std::shared_ptr<tgl_dc>& dc)
{
    auto q = std::make_shared<query_help_get_config>(std::bind(tgl_do_set_dc_configured, dc, std::placeholders::_1));
    q->out_header();
    q->out_i32(CODE_help_get_config);
    q->execute(dc, query::execution_option::FORCE);
}
/* }}} */

/* {{{ Send code */
class query_send_code: public query
{
public:
    explicit query_send_code(const std::function<void(bool, bool, const std::string&)>& callback)
        : query("send code", TYPE_TO_PARAM(auth_sent_code))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        if (m_callback) {
            tl_ds_auth_sent_code* DS_ASC = static_cast<tl_ds_auth_sent_code*>(D);
            std::string phone_code_hash = DS_STDSTR(DS_ASC->phone_code_hash);
            bool registered = DS_BVAL(DS_ASC->phone_registered);;
            m_callback(true, registered, phone_code_hash);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false, false, std::string());
        }
        return 0;
    }

    virtual void on_timeout() override
    {
        TGL_ERROR("timed out for query #" << msg_id() << " (" << name() << ")");
        if (m_callback) {
            m_callback(false, false, "TIME_OUT");
        }
    }

    virtual double timeout_interval() const override
    {
        return 20;
    }

    virtual bool should_retry_on_timeout() override
    {
        return false;
    }

    virtual void will_be_pending() override
    {
        timeout_within(timeout_interval());
    }

private:
    std::function<void(bool, bool, const std::string)> m_callback;
};

static void tgl_do_send_code(const std::string& phone, const std::function<void(bool, bool, const std::string&)>& callback)
{
    TGL_NOTICE("requesting confirmation code from dc " << tgl_state::instance()->working_dc()->id);
    auto q = std::make_shared<query_send_code>(callback);
    q->out_i32(CODE_auth_send_code);
    q->out_std_string(phone);
    q->out_i32(0);
    q->out_i32(tgl_state::instance()->app_id());
    q->out_string(tgl_state::instance()->app_hash().c_str());
    q->out_string("en");
    q->execute(tgl_state::instance()->working_dc(), query::execution_option::LOGIN);
}

class query_phone_call: public query
{
public:
    explicit query_phone_call(const std::function<void(bool)>& callback)
        : query("phone call", TYPE_TO_PARAM(bool))
        , m_callback(callback)
    { }

    virtual void on_answer(void*) override
    {
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

    virtual void on_timeout() override
    {
        TGL_ERROR("timed out for query #" << msg_id() << " (" << name() << ")");
        if (m_callback) {
            m_callback(false);
        }
    }

    virtual double timeout_interval() const override
    {
        return 20;
    }

    virtual bool should_retry_on_timeout() override
    {
        return false;
    }

    virtual void will_be_pending() override
    {
        timeout_within(timeout_interval());
    }

private:
    std::function<void(bool)> m_callback;
};

static void tgl_do_phone_call(const std::string& phone, const std::string& hash,
        const std::function<void(bool)>& callback)
{
    TGL_DEBUG("calling user at phone number: " << phone);

    auto q = std::make_shared<query_phone_call>(callback);
    q->out_header();
    q->out_i32(CODE_auth_send_call);
    q->out_std_string(phone);
    q->out_std_string(hash);
    q->execute(tgl_state::instance()->working_dc(), query::execution_option::LOGIN);
}
/* }}} */

/* {{{ Sign in / Sign up */
class query_sign_in: public query
{
public:
    explicit query_sign_in(const std::function<void(bool, const std::shared_ptr<struct tgl_user>&)>& callback)
        : query("sign in", TYPE_TO_PARAM(auth_authorization))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        TGL_DEBUG("sign_in_on_answer");
        tl_ds_auth_authorization* DS_AA = static_cast<tl_ds_auth_authorization*>(D);
        std::shared_ptr<struct tgl_user> user = tglf_fetch_alloc_user(DS_AA->user);
        tgl_state::instance()->set_dc_logged_in(tgl_state::instance()->working_dc()->id);
        if (m_callback) {
            m_callback(!!user, user);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false, nullptr);
        }
        return 0;
    }

    virtual void on_timeout() override
    {
        TGL_ERROR("timed out for query #" << msg_id() << " (" << name() << ")");
        if (m_callback) {
            m_callback(false, nullptr);
        }
    }

    virtual double timeout_interval() const override
    {
        return 20;
    }

    virtual bool should_retry_on_timeout() override
    {
        return false;
    }

    virtual void will_be_pending() override
    {
        timeout_within(timeout_interval());
    }

private:
    std::function<void(bool, const std::shared_ptr<struct tgl_user>&)> m_callback;
};

static int tgl_do_send_code_result(const std::string& phone,
        const std::string& hash,
        const std::string& code,
        const std::function<void(bool success, const std::shared_ptr<tgl_user>& U)>& callback)
{
    auto q = std::make_shared<query_sign_in>(callback);
    q->out_i32(CODE_auth_sign_in);
    q->out_std_string(phone);
    q->out_std_string(hash);
    q->out_std_string(code);
    q->execute(tgl_state::instance()->working_dc(), query::execution_option::LOGIN);
    return 0;
}

static int tgl_do_send_code_result_auth(const std::string& phone,
        const std::string& hash,
        const std::string& code,
        const std::string& first_name,
        const std::string& last_name,
        const std::function<void(bool, const std::shared_ptr<tgl_user>&)>& callback)
{
    auto q = std::make_shared<query_sign_in>(callback);
    q->out_i32(CODE_auth_sign_up);
    q->out_std_string(phone);
    q->out_std_string(hash);
    q->out_std_string(code);
    q->out_std_string(first_name);
    q->out_std_string(last_name);
    q->execute(tgl_state::instance()->working_dc(), query::execution_option::LOGIN);
    return 0;
}

static int tgl_do_send_bot_auth(const char* code, int code_len,
        const std::function<void(bool, const std::shared_ptr<tgl_user>&)>& callback)
{
    auto q = std::make_shared<query_sign_in>(callback);
    q->out_i32(CODE_auth_import_bot_authorization);
    q->out_i32(0);
    q->out_i32(tgl_state::instance()->app_id());
    q->out_std_string(tgl_state::instance()->app_hash());
    q->out_string(code, code_len);
    q->execute(tgl_state::instance()->working_dc(), query::execution_option::LOGIN);
    return 0;
}
/* }}} */

class query_logout: public query
{
public:
    explicit query_logout(const std::function<void(bool)>& callback)
        : query("logout", TYPE_TO_PARAM(bool))
        , m_callback(callback)
    { }

    virtual void on_answer(void*) override
    {
        TGL_DEBUG("logout successfully");
        tgl_do_set_dc_logged_out(dc(), true);
        if (m_callback) {
            m_callback(true);
        }
        tgl_state::instance()->callback()->logged_out(true);
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        tgl_do_set_dc_logged_out(dc(), false);
        if (m_callback) {
            m_callback(false);
        }
        tgl_state::instance()->callback()->logged_out(false);
        return 0;
    }

    virtual void on_timeout() override
    {
        TGL_ERROR("timed out for query #" << msg_id() << " (" << name() << ")");
        tgl_do_set_dc_logged_out(dc(), false);
        if (m_callback) {
            m_callback(false);
        }
    }

    virtual double timeout_interval() const override
    {
        return 20;
    }

    virtual bool should_retry_on_timeout() override
    {
        return false;
    }

    virtual void will_be_pending() override
    {
        timeout_within(timeout_interval());
    }

private:
    std::function<void(bool)> m_callback;
};

void tgl_do_logout(const std::function<void(bool)>& callback)
{
    auto dc = tgl_state::instance()->working_dc();
    if (dc->is_logging_out()) {
        return;
    }

    if (!dc->is_logged_in()) {
        if (callback) {
            callback(true);
        }
        return;
    }

    auto q = std::make_shared<query_logout>(callback);
    q->out_i32(CODE_auth_log_out);
    q->execute(dc, query::execution_option::LOGOUT);
}

/* {{{ Get contacts */
class query_get_contacts: public query
{
public:
    explicit query_get_contacts(
            const std::function<void(bool, const std::vector<std::shared_ptr<tgl_user>>&)>& callback)
        : query("get contacts", TYPE_TO_PARAM(contacts_contacts))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_contacts_contacts* DS_CC = static_cast<tl_ds_contacts_contacts*>(D);
        int n = DS_CC->users ? DS_LVAL(DS_CC->users->cnt) : 0;
        std::vector<std::shared_ptr<tgl_user>> users(n);
        for (int i = 0; i < n; i++) {
            users[i] = tglf_fetch_alloc_user(DS_CC->users->data[i]);
        }
        if (m_callback) {
            m_callback(true, users);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false, std::vector<std::shared_ptr<tgl_user>>());
        }
        return 0;
    }

private:
    std::function<void(bool, const std::vector<std::shared_ptr<tgl_user>>&)> m_callback;
};

void tgl_do_update_contact_list(const std::function<void(bool, const std::vector<std::shared_ptr<tgl_user>>&)>& callback)
{
    auto q = std::make_shared<query_get_contacts>(callback);
    q->out_i32(CODE_contacts_get_contacts);
    q->out_string("");
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ Send msg (plain text) */
class query_msg_send: public query
{
public:
    query_msg_send(const std::shared_ptr<tgl_message>& message,
            const std::function<void(bool, const std::shared_ptr<tgl_message>&)>& callback)
        : query("send message", TYPE_TO_PARAM(updates))
        , m_message(message)
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_updates* DS_U = static_cast<tl_ds_updates*>(D);
        tglu_work_any_updates(DS_U, m_message);
        if (m_callback) {
            m_callback(true, m_message);
        }
        //tgl_state::instance()->callback()->message_sent(m_message, DS_LVAL(DS_U->id), DS_LVAL(DS_U->id));
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " <<  error_code << " " << error_string);
#if 0
        tgl_message_id_t id;
        id.peer_type = TGL_PEER_RANDOM_ID;
        id.id = *(int64_t*)q->extra;
        free(q->extra, 8);
        struct tgl_message* M = tgl_message_get(&id);
        if (q->callback) {
            ((void(*)(struct tgl_state *,void *, int, struct tgl_message *))q->callback) (q->callback_extra, 0, M);
        }
        if (M) {
            bl_do_message_delete(TLS, &M->permanent_id);
        }
#endif
        m_message->set_pending(false).set_send_failed(true);

        if (m_callback) {
            m_callback(false, m_message);
        }

        // FIXME: is this correct? Maybe when we implement message deletion disabled above.
        // <--- I think it is not correct. The message will still be shown to the user and has a
        // sent error status. So the user can choose to send it again.
        //tgl_state::instance()->callback()->message_deleted(m_message->permanent_id.id);
        tgl_state::instance()->callback()->new_messages({m_message});
        return 0;
    }
private:
    std::shared_ptr<tgl_message> m_message;
    std::function<void(bool, const std::shared_ptr<tgl_message>&)> m_callback;
};

static void send_message(const std::shared_ptr<tgl_message>& M, bool disable_preview,
        const std::function<void(bool, const std::shared_ptr<tgl_message>& M)>& callback)
{
    assert(M->to_id.peer_type != tgl_peer_type::enc_chat);
    if (M->to_id.peer_type == tgl_peer_type::enc_chat) {
        TGL_WARNING("call tgl_do_send_encr_msg please");
        return;
    }
    auto q = std::make_shared<query_msg_send>(M, callback);
    q->out_i32(CODE_messages_send_message);

    unsigned f = (disable_preview ? 2 : 0) | (M->reply_id ? 1 : 0) | (M->reply_markup ? 4 : 0) | (M->entities.size() > 0 ? 8 : 0);
    if (M->from_id.peer_type == tgl_peer_type::channel) {
        f |= 16;
    }
    q->out_i32(f);
    q->out_input_peer(M->to_id);
    if (M->reply_id) {
        q->out_i32(M->reply_id);
    }
    q->out_std_string(M->message);
    q->out_i64(M->permanent_id);

    //TODO
    //int64_t* x = (int64_t*)malloc(12);
    //*x = M->id;
    //*(int*)(x+1) = M->to_id.id;

    if (M->reply_markup) {
        if (!M->reply_markup->button_matrix.empty()) {
            q->out_i32(CODE_reply_keyboard_markup);
            q->out_i32(M->reply_markup->flags);
            q->out_i32(CODE_vector);
            q->out_i32(M->reply_markup->button_matrix.size());
            for (size_t i = 0; i < M->reply_markup->button_matrix.size(); ++i) {
                q->out_i32(CODE_keyboard_button_row);
                q->out_i32(CODE_vector);
                q->out_i32(M->reply_markup->button_matrix[i].size());
                for (size_t j = 0; j < M->reply_markup->button_matrix[i].size(); ++j) {
                    q->out_i32(CODE_keyboard_button);
                    q->out_std_string(M->reply_markup->button_matrix[i][j]);
                }
            }
        } else {
            q->out_i32(CODE_reply_keyboard_hide);
        }
    }

    if (M->entities.size() > 0) {
        q->out_i32(CODE_vector);
        q->out_i32(M->entities.size());
        for (size_t i = 0; i < M->entities.size(); i++) {
            auto entity = M->entities[i];
            switch (entity->type) {
            case tgl_message_entity_type::bold:
                q->out_i32(CODE_message_entity_bold);
                q->out_i32(entity->start);
                q->out_i32(entity->length);
                break;
            case tgl_message_entity_type::italic:
                q->out_i32(CODE_message_entity_italic);
                q->out_i32(entity->start);
                q->out_i32(entity->length);
                break;
            case tgl_message_entity_type::code:
                q->out_i32(CODE_message_entity_code);
                q->out_i32(entity->start);
                q->out_i32(entity->length);
                break;
            case tgl_message_entity_type::text_url:
                q->out_i32(CODE_message_entity_text_url);
                q->out_i32(entity->start);
                q->out_i32(entity->length);
                q->out_std_string(entity->text_url);
                break;
            default:
                assert(0);
            }
        }
    }

    q->execute(tgl_state::instance()->working_dc());
}

int64_t tgl_do_send_message(const tgl_input_peer_t& peer_id,
        const std::string& text,
        int32_t reply_id,
        bool disable_preview,
        bool post_as_channel_message,
        const std::shared_ptr<tl_ds_reply_markup>& reply_markup,
        const std::function<void(bool success, const std::shared_ptr<tgl_message>& message)>& callback)
{
    std::shared_ptr<tgl_secret_chat> secret_chat;
    if (peer_id.peer_type == tgl_peer_type::enc_chat) {
        secret_chat = tgl_state::instance()->secret_chat_for_id(peer_id);
        if (!secret_chat) {
            TGL_ERROR("unknown secret chat");
            if (callback) {
                callback(false, nullptr);
            }
            return 0;
        }
        if (secret_chat->state != tgl_secret_chat_state::ok) {
            TGL_ERROR("secret chat not in ok state");
            if (callback) {
                callback(false, nullptr);
            }
            return 0;
        }
    }

    int64_t date = tgl_get_system_time();

    int64_t message_id;
    do {
        tgl_secure_random(reinterpret_cast<unsigned char*>(&message_id), 8);
    } while (!message_id);

    std::shared_ptr<tgl_message> message;

    if (peer_id.peer_type != tgl_peer_type::enc_chat) {
        struct tl_ds_message_media TDSM;
        TDSM.magic = CODE_message_media_empty;

        tgl_peer_id_t from_id;
        if (post_as_channel_message) {
            from_id = tgl_peer_id_t::from_input_peer(peer_id);
        } else {
            from_id = tgl_state::instance()->our_id();
        }

        message = tglm_create_message(message_id, from_id, peer_id, nullptr, nullptr, &date, text, &TDSM, nullptr, reply_id, reply_markup.get());
        message->set_unread(true).set_outgoing(true).set_pending(true);
        tgl_state::instance()->callback()->new_messages({message});

        send_message(message, disable_preview, callback);
    } else {
        struct tl_ds_decrypted_message_media TDSM;
        TDSM.magic = CODE_decrypted_message_media_empty;

        tgl_peer_id_t from_id = tgl_state::instance()->our_id();

        assert(secret_chat);
        message = tglm_create_encr_message(secret_chat, message_id, from_id, peer_id, &date, text, &TDSM, nullptr, nullptr, true);
        message->set_unread(true).set_pending(true);
        tgl_do_send_encr_msg(secret_chat, message, callback);
        tgl_state::instance()->callback()->new_messages({message});
    }

    return message_id;
}

class query_mark_read: public query
{
public:
    query_mark_read(const tgl_input_peer_t& id, int max_id,
            const std::function<void(bool)>& callback)
        : query("mark read", id.peer_type == tgl_peer_type::channel ? TYPE_TO_PARAM(bool) : TYPE_TO_PARAM(messages_affected_messages))
        , m_id(id)
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        if (m_id.peer_type == tgl_peer_type::channel) {
            if (m_callback) {
                m_callback(true);
            }
            // FIXME: should we call messages_mark_read_in() callback? What should we pass for msg_id?
            return;
        }

        tl_ds_messages_affected_messages* DS_MAM = static_cast<tl_ds_messages_affected_messages*>(D);

        if (tgl_check_pts_diff(DS_LVAL(DS_MAM->pts), DS_LVAL(DS_MAM->pts_count))) {
            tgl_state::instance()->set_pts(DS_LVAL(DS_MAM->pts));
        }
        if (tgl_state::instance()->callback()) {
            tgl_state::instance()->callback()->messages_mark_read_in(tgl_peer_id_t::from_input_peer(m_id), DS_LVAL(DS_MAM->pts));
        }
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

    virtual double timeout_interval() const override
    {
        return 120;
    }

private:
    tgl_input_peer_t m_id;
    std::function<void(bool)> m_callback;
};

void tgl_do_message_mark_read_encrypted(const tgl_input_peer_t& id, int32_t max_time, const std::function<void(bool success)>& callback)
{
    if (id.peer_type == tgl_peer_type::user || id.peer_type == tgl_peer_type::chat || id.peer_type == tgl_peer_type::channel) {
        return;
    }
    assert(id.peer_type == tgl_peer_type::enc_chat);
    std::shared_ptr<tgl_secret_chat> secret_chat = tgl_state::instance()->secret_chat_for_id(id);
    if (!secret_chat) {
        TGL_ERROR("unknown secret chat");
        if (callback) {
            callback(false);
        }
        return;
    }
    tgl_do_messages_mark_read_encr(secret_chat, max_time, nullptr);
}

void tgl_do_mark_read(const tgl_input_peer_t& id, int max_id_or_time,
        const std::function<void(bool)>& callback)
{
    //if (tgl_state::instance()->is_bot) { return; }
    if (id.peer_type == tgl_peer_type::enc_chat) {
        tgl_do_message_mark_read_encrypted(id, max_id_or_time, callback);
        return;
    }

    if (id.peer_type != tgl_peer_type::channel) {
        auto q = std::make_shared<query_mark_read>(id, max_id_or_time, callback);
        q->out_i32(CODE_messages_read_history);
        q->out_input_peer(id);
        q->out_i32(max_id_or_time);
        //q->out_i32(offset);
        q->execute(tgl_state::instance()->working_dc());
    } else {
        auto q = std::make_shared<query_mark_read>(id, max_id_or_time, callback);
        q->out_i32(CODE_channels_read_history);
        q->out_i32(CODE_input_channel);
        q->out_i32(id.peer_id);
        q->out_i64(id.access_hash);
        q->out_i32(max_id_or_time);
        q->execute(tgl_state::instance()->working_dc());
    }
}

/* {{{ Get history */
class query_get_history: public query
{
public:
    query_get_history(const tgl_input_peer_t& id, int limit, int offset, int max_id,
            const std::function<void(bool, const std::vector<std::shared_ptr<tgl_message>>&)>& callback)
        : query("get history", TYPE_TO_PARAM(messages_messages))
        , m_id(id)
        //, m_limit(limit)
        //, m_offset(offset)
        //, m_max_id(max_id)
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        TGL_DEBUG("get history on answer for query #" << msg_id());
        tl_ds_messages_messages* DS_MM = static_cast<tl_ds_messages_messages*>(D);
        for (int i = 0; i < DS_LVAL(DS_MM->chats->cnt); i++) {
            tglf_fetch_alloc_chat(DS_MM->chats->data[i]);
        }

        for (int i = 0; i < DS_LVAL(DS_MM->users->cnt); i++) {
            tglf_fetch_alloc_user(DS_MM->users->data[i]);
        }

        int n = DS_LVAL(DS_MM->messages->cnt);
        for (int i = 0; i < n; i++) {
            auto msg = tglf_fetch_alloc_message(DS_MM->messages->data[i]);
            msg->set_history(true);
            m_messages.push_back(msg);
        }

        tgl_state::instance()->callback()->new_messages(m_messages);
//        m_offset += n;
//        m_limit -= n;

//        int count = DS_LVAL(DS_MM->count);
//        if (count >= 0 && m_limit + m_offset >= count) {
//            m_limit = count - m_offset;
//            if (m_limit < 0) {
//                m_limit = 0;
//            }
//        }
        //assert(m_limit >= 0);

        if (m_callback) {
            m_callback(true, m_messages);
        }
//        if (m_limit <= 0 || DS_MM->magic == CODE_messages_messages || DS_MM->magic == CODE_messages_channel_messages) {

//            /*if (m_messages.size() > 0) {
//              tgl_do_messages_mark_read(m_id, m_messages[0]->id, 0, 0, 0);
//            }*/
//        } else {
//            /*m_offset = 0;
//            m_max_id = m_messages[m_messages.size()-1]->permanent_id.id;
//            _tgl_do_get_history(m_id, m_offset, m_limit, m_max_id,
//                    m_callback);*/
//        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false, std::vector<std::shared_ptr<tgl_message>>());
        }
        return 0;
    }
private:
    std::vector<std::shared_ptr<tgl_message>> m_messages;
    tgl_input_peer_t m_id;
//    int m_limit;
//    int m_offset;
//    int m_max_id;
    std::function<void(bool, const std::vector<std::shared_ptr<tgl_message>>&)> m_callback;
};

void tgl_do_get_history(const tgl_input_peer_t& id, int offset, int limit,
        const std::function<void(bool, const std::vector<std::shared_ptr<tgl_message>>& list)>& callback) {
    assert(id.peer_type != tgl_peer_type::enc_chat);
    auto q = std::make_shared<query_get_history>(id, limit, offset, 0/*max_id*/, callback);
    q->out_i32(CODE_messages_get_history);
    q->out_input_peer(id);
    q->out_i32(0); // offset_id
    q->out_i32(offset); // add_offset
    q->out_i32(limit);
    q->out_i32(0); // max_id
    q->out_i32(0); // min_id
    q->execute(tgl_state::instance()->working_dc());
}

/* }}} */

/* {{{ Get dialogs */
struct get_dialogs_state {
    std::vector<tgl_peer_id_t> peers;
    std::vector<int64_t> last_message_ids;
    std::vector<int> unread_count;
    std::vector<int> read_box_max_id;
    tgl_peer_id_t offset_peer;
    int limit = 0;
    int offset = 0;
    int offset_date;
    int max_id = 0;
    int channels = 0;
};

static void tgl_do_get_dialog_list(const std::shared_ptr<get_dialogs_state>& state,
        const std::function<void(bool, const std::vector<tgl_peer_id_t>&, const std::vector<int64_t>&, const std::vector<int>&)>& callback);

class query_get_dialogs: public query
{
public:
    query_get_dialogs(const std::shared_ptr<get_dialogs_state>& state,
            const std::function<void(bool, const std::vector<tgl_peer_id_t>&, const std::vector<int64_t>&, const std::vector<int>&)>& callback)
        : query("get dialogs", TYPE_TO_PARAM(messages_dialogs))
        , m_state(state)
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_messages_dialogs* DS_MD = static_cast<tl_ds_messages_dialogs*>(D);
        int dl_size = DS_LVAL(DS_MD->dialogs->cnt);

        for (int i = 0; i < DS_LVAL(DS_MD->chats->cnt); i++) {
            tglf_fetch_alloc_chat(DS_MD->chats->data[i]);
        }

        for (int i = 0; i < DS_LVAL(DS_MD->users->cnt); i++) {
            tglf_fetch_alloc_user(DS_MD->users->data[i]);
        }

        for (int i = 0; i < dl_size; i++) {
            struct tl_ds_dialog* DS_D = DS_MD->dialogs->data[i];
            tgl_peer_id_t peer_id = tglf_fetch_peer_id(DS_D->peer);
            m_state->peers.push_back(peer_id);
            m_state->last_message_ids.push_back(DS_LVAL(DS_D->top_message));
            m_state->unread_count.push_back(DS_LVAL(DS_D->unread_count));
            m_state->read_box_max_id.push_back(DS_LVAL(DS_D->read_inbox_max_id));
        }

        std::vector<std::shared_ptr<tgl_message>> new_messages;
        for (int i = 0; i < DS_LVAL(DS_MD->messages->cnt); i++) {
            new_messages.push_back(tglf_fetch_alloc_message(DS_MD->messages->data[i]));
        }
        tgl_state::instance()->callback()->new_messages(new_messages);

        TGL_DEBUG("dl_size = " << dl_size << ", total = " << m_state->peers.size());

        if (dl_size && static_cast<int>(m_state->peers.size()) < m_state->limit
                && DS_MD->magic == CODE_messages_dialogs_slice
                && static_cast<int>(m_state->peers.size()) < DS_LVAL(DS_MD->count)) {
            if (m_state->peers.size() > 0) {
                m_state->offset_peer = m_state->peers[m_state->peers.size() - 1];
#if 0
                int p = static_cast<int>(m_state->size()) - 1;
                while (p >= 0) {
                    struct tgl_message* M = tgl_message_get(m_state->last_message_ids[p]);
                    if (M) {
                        m_state->offset_date = M->date;
                        break;
                    }
                    p --;
                }
#endif
            }
            tgl_do_get_dialog_list(m_state, m_callback);
        } else {
            if (m_callback) {
                m_callback(true, m_state->peers, m_state->last_message_ids, m_state->unread_count);
            }
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_code);
        if (m_callback) {
            m_callback(0, std::vector<tgl_peer_id_t>(), std::vector<int64_t>(), std::vector<int>());
        }
        return 0;
    }

private:
    std::shared_ptr<get_dialogs_state> m_state;
    std::function<void(bool, const std::vector<tgl_peer_id_t>&,
            const std::vector<int64_t>&, const std::vector<int>&)> m_callback;
};

static void tgl_do_get_dialog_list(const std::shared_ptr<get_dialogs_state>& state,
        const std::function<void(bool, const std::vector<tgl_peer_id_t>&, const std::vector<int64_t>&, const std::vector<int>&)>& callback)
{
    auto q = std::make_shared<query_get_dialogs>(state, callback);
    if (state->channels) {
        q->out_i32(CODE_channels_get_dialogs);
        q->out_i32(state->offset);
        q->out_i32(state->limit - state->peers.size());
    } else {
        q->out_i32(CODE_messages_get_dialogs);
        q->out_i32(state->offset_date);
        q->out_i32(state->offset);
        //q->out_i32(0);
        if (state->offset_peer.peer_type != tgl_peer_type::unknown) {
            q->out_peer_id(state->offset_peer, 0); // FIXME: do we need an access_hash?
        } else {
            q->out_i32(CODE_input_peer_empty);
        }
        q->out_i32(state->limit - state->peers.size());
    }
    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_get_dialog_list(int limit, int offset,
        const std::function<void(bool success,
                const std::vector<tgl_peer_id_t>& peers,
                const std::vector<int64_t>& last_msg_ids,
                const std::vector<int>& unread_count)>& callback)
{
    std::shared_ptr<get_dialogs_state> state = std::make_shared<get_dialogs_state>();
    state->limit = limit;
    state->offset = offset;
    state->channels = 0;
    tgl_do_get_dialog_list(state, callback);
}

void tgl_do_get_channels_dialog_list(int limit, int offset,
        const std::function<void(bool success,
                const std::vector<tgl_peer_id_t>& peers,
                const std::vector<int64_t>& last_msg_ids,
                const std::vector<int>& unread_count)>& callback)
{
    std::shared_ptr<get_dialogs_state> state = std::make_shared<get_dialogs_state>();
    state->limit = limit;
    state->offset = offset;
    state->channels = 1;
    state->offset_date = 0;
    state->offset_peer.peer_type = tgl_peer_type::unknown;
    tgl_do_get_dialog_list(state, callback);
}
/* }}} */

/* {{{ Profile name */
class query_set_profile_name: public query
{
public:
    explicit query_set_profile_name(const std::function<void(bool)>& callback)
        : query("set profile name", TYPE_TO_PARAM(user))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tglf_fetch_alloc_user(static_cast<tl_ds_user*>(D));
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_code);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

void tgl_do_set_profile_name(const std::string& first_name, const std::string& last_name,
        const std::function<void(bool)>& callback)
{
    auto q = std::make_shared<query_set_profile_name>(callback);
    q->out_i32(CODE_account_update_profile);
    q->out_string(first_name.c_str(), last_name.length());
    q->out_string(last_name.c_str(), last_name.length());
    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_set_username(const std::string& username, const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_set_profile_name>(callback);
    q->out_i32(CODE_account_update_username);
    q->out_string(username.c_str(), username.length());
    q->execute(tgl_state::instance()->working_dc());
}

class query_check_username: public query
{
public:
    explicit query_check_username(const std::function<void(int)>& callback)
        : query("check username", TYPE_TO_PARAM(bool))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        auto value = static_cast<tl_ds_bool*>(D);
        if (m_callback) {
            // 0: user name valid and available
            // 1: user name is already taken
            m_callback(value->magic == CODE_bool_true ? 0 : 1);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            if (error_code == 400) {
                // user name invalid
                m_callback(2);
            } else if (error_code == 600) {
                // not connected
                m_callback(3);
            }
        }
        return 0;
    }

private:
    std::function<void(int)> m_callback;
};

void tgl_do_check_username(const std::string& username, const std::function<void(int result)>& callback)
{
    auto q = std::make_shared<query_check_username>(callback);
    q->out_i32(CODE_account_check_username);
    q->out_string(username.c_str(), username.length());
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ Contacts search */
class query_contacts_search: public query
{
public:
    explicit query_contacts_search(const std::function<void(const std::vector<std::shared_ptr<tgl_user>>&,
            const std::vector<std::shared_ptr<tgl_chat>>&)> callback)
        : query("contact search", TYPE_TO_PARAM(contacts_found))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_contacts_found* DS_CRU = static_cast<tl_ds_contacts_found*>(D);
        std::vector<std::shared_ptr<tgl_user>> users;
        for (int i = 0; i < DS_LVAL(DS_CRU->users->cnt); i++) {
            users.push_back(tglf_fetch_alloc_user(DS_CRU->users->data[i], false));
        }
        std::vector<std::shared_ptr<tgl_chat>> chats;
        for (int i = 0; i < DS_LVAL(DS_CRU->chats->cnt); i++) {
            chats.push_back(tglf_fetch_alloc_chat(DS_CRU->chats->data[i], false));
        }
        if (m_callback) {
            m_callback(users, chats);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_code);
        if (m_callback) {
            m_callback({},{});
        }
        return 0;
    }

private:
    std::function<void(const std::vector<std::shared_ptr<tgl_user>>&,
            const std::vector<std::shared_ptr<tgl_chat>>&)> m_callback;
};

void tgl_do_contact_search(const std::string& name, int limit,
        const std::function<void(const std::vector<std::shared_ptr<tgl_user>>&,
                           const std::vector<std::shared_ptr<tgl_chat>>&)>& callback)
{
    auto q = std::make_shared<query_contacts_search>(callback);
    q->out_i32(CODE_contacts_search);
    q->out_string(name.c_str(), name.length());
    q->out_i32(limit);
    q->execute(tgl_state::instance()->working_dc());
}

class query_contact_resolve_username: public query
{
public:
    explicit query_contact_resolve_username(const std::function<void(bool)>& callback)
        : query("contact resolve username", TYPE_TO_PARAM(contacts_resolved_peer))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_contacts_resolved_peer* DS_CRU = static_cast<tl_ds_contacts_resolved_peer*>(D);
        //tgl_peer_id_t peer_id = tglf_fetch_peer_id(DS_CRU->peer);
        for (int i = 0; i < DS_LVAL(DS_CRU->users->cnt); i++) {
            tglf_fetch_alloc_user(DS_CRU->users->data[i]);
        }
        for (int i = 0; i < DS_LVAL(DS_CRU->chats->cnt); i++) {
            tglf_fetch_alloc_chat(DS_CRU->chats->data[i]);
        }
        //tgl_peer_t* P = tgl_peer_get(peer_id);
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_code);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

void tgl_do_contact_resolve_username(const std::string& name, const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_contact_resolve_username>(callback);
    q->out_i32(CODE_contacts_resolve_username);
    q->out_string(name.c_str(), name.length());
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ Forward */
query_send_msgs::query_send_msgs(const std::shared_ptr<messages_send_extra>& extra,
        const std::function<void(bool, const std::shared_ptr<tgl_message>&)>& single_callback)
    : query("send messages (single)", TYPE_TO_PARAM(updates))
    , m_extra(extra)
    , m_single_callback(single_callback)
    , m_multi_callback(nullptr)
    , m_bool_callback(nullptr)
    , m_message(nullptr)
{
    assert(m_extra);
    assert(!m_extra->multi);
}

query_send_msgs::query_send_msgs(const std::shared_ptr<messages_send_extra>& extra,
        const std::function<void(bool success, const std::vector<std::shared_ptr<tgl_message>>& messages)>& multi_callback)
    : query("send messages (multi)", TYPE_TO_PARAM(updates))
    , m_extra(extra)
    , m_single_callback(nullptr)
    , m_multi_callback(multi_callback)
    , m_bool_callback(nullptr)
    , m_message(nullptr)
{
    assert(m_extra);
    assert(m_extra->multi);
}

query_send_msgs::query_send_msgs(const std::function<void(bool)>& bool_callback)
    : query("send messages (bool callback)", TYPE_TO_PARAM(updates))
    , m_extra(nullptr)
    , m_single_callback(nullptr)
    , m_multi_callback(nullptr)
    , m_bool_callback(bool_callback)
    , m_message(nullptr)
{ }

void query_send_msgs::on_answer(void* D)
{
    tl_ds_updates* DS_U = static_cast<tl_ds_updates*>(D);

    tglu_work_any_updates(DS_U, m_message);

    if (!m_extra) {
        if (m_bool_callback) {
            m_bool_callback(true);
        }
    } else if (m_extra->multi) {
        std::vector<std::shared_ptr<tgl_message>> messages;
#if 0 // FIXME
        int count = E->count;
        int i;
        for (i = 0; i < count; i++) {
            int y = tgls_get_local_by_random(E->message_ids[i]);
            ML[i] = tgl_message_get(y);
        }
#endif
        if (m_multi_callback) {
            m_multi_callback(true, messages);
        }
    } else {
#if 0 // FIXME
        int y = tgls_get_local_by_random(E->id);
        struct tgl_message* M = tgl_message_get(y);
#endif
        std::shared_ptr<tgl_message> M;
        if (m_single_callback) {
            m_single_callback(true, M);
        }
    }
}

int query_send_msgs::on_error(int error_code, const std::string& error_string)
{
    TGL_ERROR("RPC_CALL_FAIL " <<  error_code << " " << error_string);

    if (!m_extra) {
        if (m_bool_callback) {
            m_bool_callback(false);
        }
    } else if (m_extra->multi) {
        if (m_multi_callback) {
            m_multi_callback(false, std::vector<std::shared_ptr<tgl_message>>());
        }
    } else {
        if (m_single_callback) {
            m_single_callback(false, nullptr);
        }
    }
    return 0;
}

void query_send_msgs::set_message(const std::shared_ptr<tgl_message>& message)
{
    m_message = message;
}

void tgl_do_forward_messages(const tgl_input_peer_t& from_id, const tgl_input_peer_t& to_id,
        const std::vector<int64_t>& message_ids, bool post_as_channel_message,
        const std::function<void(bool success, const std::vector<std::shared_ptr<tgl_message>>& messages)>& callback)
{
    if (to_id.peer_type == tgl_peer_type::enc_chat) {
        TGL_ERROR("can not forward messages to secret chats");
        if (callback) {
            callback(false, std::vector<std::shared_ptr<tgl_message>>());
        }
        return;
    }

    std::shared_ptr<messages_send_extra> E = std::make_shared<messages_send_extra>();
    E->multi = true;
    E->count = message_ids.size();

    auto q = std::make_shared<query_send_msgs>(E, callback);
    q->out_i32(CODE_messages_forward_messages);

    unsigned f = 0;
    if (post_as_channel_message) {
        f |= 16;
    }
    q->out_i32(f);
    q->out_input_peer(from_id);
    q->out_i32(CODE_vector);
    q->out_i32(message_ids.size());
    for (size_t i = 0; i < message_ids.size(); i++) {
        q->out_i32(message_ids[i]);
    }

    q->out_i32(CODE_vector);
    q->out_i32(message_ids.size());
    for (size_t i = 0; i < message_ids.size(); i++) {
        int64_t new_message_id;
        tgl_secure_random(reinterpret_cast<unsigned char*>(&new_message_id), 8);
        E->message_ids.push_back(new_message_id);
        q->out_i64(new_message_id);
    }
    q->out_input_peer(to_id);
    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_forward_message(const tgl_input_peer_t& from_id, const tgl_input_peer_t& to_id, int64_t message_id,
        const std::function<void(bool success, const std::shared_ptr<tgl_message>& M)>& callback)
{
    if (from_id.peer_type == tgl_peer_type::temp_id) {
        TGL_ERROR("unknown message");
        if (callback) {
            callback(false, nullptr);
        }
        return;
    }
    if (from_id.peer_type == tgl_peer_type::enc_chat) {
        TGL_ERROR("can not forward messages from secret chat");
        if (callback) {
            callback(false, nullptr);
        }
        return;
    }
    if (to_id.peer_type == tgl_peer_type::enc_chat) {
        TGL_ERROR("can not forward messages to secret chats");
        if (callback) {
            callback(false, nullptr);
        }
        return;
    }

    std::shared_ptr<messages_send_extra> E = std::make_shared<messages_send_extra>();
    tgl_secure_random(reinterpret_cast<unsigned char*>(&E->id), 8);
    auto q = std::make_shared<query_send_msgs>(E, callback);
    q->out_i32(CODE_messages_forward_message);
    q->out_input_peer(from_id);
    q->out_i32(message_id);

    q->out_i64(E->id);
    q->out_input_peer(to_id);
    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_send_contact(const tgl_input_peer_t& id,
      const std::string& phone, const std::string& first_name, const std::string& last_name, int32_t reply_id,
      const std::function<void(bool success, const std::shared_ptr<tgl_message>& M)>& callback)
{
    if (id.peer_type == tgl_peer_type::enc_chat) {
        TGL_ERROR("can not send contact to secret chat");
        if (callback) {
            callback(false, nullptr);
        }
        return;
    }

    std::shared_ptr<messages_send_extra> E = std::make_shared<messages_send_extra>();
    tgl_secure_random(reinterpret_cast<unsigned char*>(&E->id), 8);

    auto q = std::make_shared<query_send_msgs>(E, callback);
    q->out_i32(CODE_messages_send_media);
    q->out_i32(reply_id ? 1 : 0);
    if (reply_id) {
        q->out_i32(reply_id);
    }
    q->out_input_peer(id);
    q->out_i32(CODE_input_media_contact);
    q->out_std_string(phone);
    q->out_std_string(first_name);
    q->out_std_string(last_name);

    q->out_i64(E->id);

    q->execute(tgl_state::instance()->working_dc());
}

//void tgl_do_reply_contact(tgl_message_id_t *_reply_id, const std::string& phone, const std::string& first_name, const std::string& last_name,
//        unsigned long long flags, std::function<void(bool success, const std::shared_ptr<tgl_message>& M, float progress)> callback)
//{
//  tgl_message_id_t reply_id = *_reply_id;
//  if (reply_id.peer_type == tgl_peer_type::temp_id) {
//    TGL_ERROR("unknown message");
//    if (callback) {
//      callback(0, 0, 0);
//    }
//    return;
//  }
//  if (reply_id.peer_type == tgl_peer_type::enc_chat) {
//    TGL_ERROR("can not reply on message from secret chat");
//    if (callback) {
//      callback(0, 0, 0);
//    }

//    tgl_peer_id_t peer_id = tgl_msg_id_to_peer_id(reply_id);

//    tgl_do_send_contact(peer_id, phone, first_name, last_name, flags | TGL_SEND_MSG_FLAG_REPLY(reply_id.id), callback);
//  }
//}

void tgl_do_forward_media(const tgl_input_peer_t& to_id, int64_t message_id, bool post_as_channel_message,
        const std::function<void(bool success, const std::shared_ptr<tgl_message>& M)>& callback)
{
    if (to_id.peer_type == tgl_peer_type::enc_chat) {
        TGL_ERROR("can not forward messages to secret chats");
        if (callback) {
            callback(false, nullptr);
        }
        return;
    }
#if 0
    struct tgl_message* M = tgl_message_get(&msg_id);
    if (!M || !(M->flags & TGLMF_CREATED) || (M->flags & TGLMF_ENCRYPTED)) {
        if (!M || !(M->flags & TGLMF_CREATED)) {
            TGL_ERROR("unknown message");
        } else {
            TGL_ERROR("can not forward message from secret chat");
        }
        if (callback) {
            callback(false, nullptr);
        }
        return;
    }
    if (M->media.type != tgl_message_media_photo && M->media.type != tgl_message_media_document && M->media.type != tgl_message_media_audio && M->media.type != tgl_message_media_video) {
        TGL_ERROR("can only forward photo/document");
        if (callback) {
            callback(false, nullptr);
        }
        return;
    }
#endif
    std::shared_ptr<messages_send_extra> E = std::make_shared<messages_send_extra>();
    tgl_secure_random(reinterpret_cast<unsigned char*>(&E->id), 8);

    auto q = std::make_shared<query_send_msgs>(E, callback);
    q->out_i32(CODE_messages_send_media);
    int f = 0;
    if (post_as_channel_message) {
        f |= 16;
    }
    q->out_i32(f);
    q->out_input_peer(to_id);
#if 0
    switch (M->media.type) {
    case tgl_message_media_photo:
        assert(M->media.photo);
        out_i32(CODE_input_media_photo);
        out_i32(CODE_input_photo);
        out_i64(M->media.photo->id);
        out_i64(M->media.photo->access_hash);
        out_string("");
        break;
    case tgl_message_media_document:
    case tgl_message_media_audio:
    case tgl_message_media_video:
        assert(M->media.document);
        out_i32(CODE_input_media_document);
        out_i32(CODE_input_document);
        out_i64(M->media.document->id);
        out_i64(M->media.document->access_hash);
        out_string("");
        break;
    default:
       assert(0);
    }
#endif

  q->out_i64(E->id);
  q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ Send location */

void tgl_do_send_location(const tgl_input_peer_t& peer_id, double latitude, double longitude, int32_t reply_id, bool post_as_channel_message,
        const std::function<void(bool success, const std::shared_ptr<tgl_message>& M)>& callback)
{
    if (peer_id.peer_type == tgl_peer_type::enc_chat) {
        tgl_do_send_location_encr(peer_id, latitude, longitude, callback);
    } else {
        std::shared_ptr<messages_send_extra> E = std::make_shared<messages_send_extra>();
        tgl_secure_random(reinterpret_cast<unsigned char*>(&E->id), 8);

        auto q = std::make_shared<query_send_msgs>(E, callback);
        q->out_i32(CODE_messages_send_media);
        unsigned f = reply_id ? 1 : 0;
        if (post_as_channel_message) {
            f |= 16;
        }
        q->out_i32(f);
        if (reply_id) {
            q->out_i32(reply_id);
        }
        q->out_input_peer(peer_id);
        q->out_i32(CODE_input_media_geo_point);
        q->out_i32(CODE_input_geo_point);
        q->out_double(latitude);
        q->out_double(longitude);

        q->out_i64(E->id);

        q->execute(tgl_state::instance()->working_dc());
    }
}

#if 0
void tgl_do_reply_location(tgl_message_id_t *_reply_id, double latitude, double longitude, unsigned long long flags, std::function<void(bool success, struct tgl_message* M)> callback) {
  tgl_message_id_t reply_id = *_reply_id;
  if (reply_id.peer_type == tgl_peer_type::temp_id) {
    reply_id = tgl_convert_temp_msg_id(reply_id);
  }
  if (reply_id.peer_type == tgl_peer_type::temp_id) {
    TGL_ERROR("unknown message");
    if (callback) {
      callback(0, 0);
    }
    return;
  }
  if (reply_id.peer_type == tgl_peer_type::enc_chat) {
    TGL_ERROR("can not reply on message from secret chat");
    if (callback) {
      callback(0, 0);
    }

  tgl_peer_id_t peer_id = tgl_msg_id_to_peer_id(reply_id);

  tgl_do_send_location(peer_id, latitude, longitude, flags | TGL_SEND_MSG_FLAG_REPLY(reply_id.id), callback, callback_extra);
}
#endif
/* }}} */

/* {{{ Rename chat */

void tgl_do_rename_chat(const tgl_peer_id_t& id, const std::string& name,
        const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_send_msgs>(callback);
    q->out_i32(CODE_messages_edit_chat_title);
    assert(id.peer_type == tgl_peer_type::chat);
    q->out_i32(id.peer_id);
    q->out_std_string(name);
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

 /* {{{ Rename channel */

void tgl_do_rename_channel(const tgl_input_peer_t& id, const char* name, int name_len,
        const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_send_msgs>(callback);
    q->out_i32(CODE_channels_edit_title);
    assert(id.peer_type == tgl_peer_type::channel);
    q->out_i32(CODE_input_channel);
    q->out_i32(id.peer_id);
    q->out_i64(id.access_hash);
    q->out_string(name, name_len);
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

void tgl_do_join_channel(const tgl_input_peer_t& id, const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_send_msgs>(callback);
    q->out_i32(CODE_channels_join_channel);
    assert(id.peer_type == tgl_peer_type::channel);
    q->out_i32(CODE_input_channel);
    q->out_i32(id.peer_id);
    q->out_i64(id.access_hash);
    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_leave_channel(const tgl_input_peer_t& id, const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_send_msgs>(callback);
    q->out_i32(CODE_channels_leave_channel);
    assert(id.peer_type == tgl_peer_type::channel);
    q->out_i32(CODE_input_channel);
    q->out_i32(id.peer_id);
    q->out_i64(id.access_hash);
    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_delete_channel(const tgl_input_peer_t& channel_id, const std::function<void(bool success)>& callback)
{
    std::shared_ptr<messages_send_extra> extra = std::make_shared<messages_send_extra>();
    extra->multi = true;
    auto q = std::make_shared<query_send_msgs>(extra, [=](bool success, const std::vector<std::shared_ptr<tgl_message>>&) {
        if (callback) {
            callback(success);
        }
    });
    q->out_i32(CODE_channels_delete_channel);
    assert(channel_id.peer_type == tgl_peer_type::channel);
    q->out_i32(CODE_input_channel);
    q->out_i32(channel_id.peer_id);
    q->out_i64(channel_id.access_hash);
    q->execute(tgl_state::instance()->working_dc());
}

class query_channels_set_about: public query
{
public:
    explicit query_channels_set_about(const std::function<void(bool)>& callback)
        : query("channels set about", TYPE_TO_PARAM(bool))
        , m_callback(callback)
    { }

    virtual void on_answer(void*) override
    {
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

void tgl_do_channel_set_about(const tgl_input_peer_t& id, const char* about, int about_len,
        const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_channels_set_about>(callback);
    q->out_i32(CODE_channels_edit_about);
    assert(id.peer_type == tgl_peer_type::channel);
    q->out_i32(CODE_input_channel);
    q->out_i32(id.peer_id);
    q->out_i64(id.access_hash);
    q->out_string(about, about_len);
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ Channel set username */
void tgl_do_channel_set_username(const tgl_input_peer_t& id, const char* username, int username_len,
        const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_channels_set_about>(callback);
    q->out_i32(CODE_channels_update_username);
    assert(id.peer_type == tgl_peer_type::channel);
    q->out_i32(CODE_input_channel);
    q->out_i32(id.peer_id);
    q->out_i64(id.access_hash);
    q->out_string(username, username_len);
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ Channel set admin */
void tgl_do_channel_set_admin(const tgl_input_peer_t& channel_id, const tgl_input_peer_t& user_id, int type,
        const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_send_msgs>(callback);
    q->out_i32(CODE_channels_edit_admin);
    assert(channel_id.peer_type == tgl_peer_type::channel);
    assert(user_id.peer_type == tgl_peer_type::user);
    q->out_i32(CODE_input_channel);
    q->out_i32(channel_id.peer_id);
    q->out_i64(channel_id.access_hash);
    q->out_i32(CODE_input_user);
    q->out_i32(user_id.peer_id);
    q->out_i64(user_id.access_hash);
    switch (type) {
    case 1:
        q->out_i32(CODE_channel_role_moderator);
        break;
    case 2:
        q->out_i32(CODE_channel_role_editor);
        break;
    default:
        q->out_i32(CODE_channel_role_empty);
        break;
    }

    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

struct channel_get_participants_state {
    tgl_input_peer_t channel_id;
    std::vector<std::shared_ptr<tgl_channel_participant>> participants;
    tgl_channel_participant_type type = tgl_channel_participant_type::recent;
    int offset = 0;
    int limit = -1;
};

static void tgl_do_get_channel_participants(const std::shared_ptr<struct channel_get_participants_state>& state,
        const std::function<void(bool)>& callback);

class query_channels_get_participants: public query
{
public:
    query_channels_get_participants(const std::shared_ptr<channel_get_participants_state>& state,
            const std::function<void(bool)>& callback)
        : query("channels get participants", TYPE_TO_PARAM(channels_channel_participants))
        , m_state(state)
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_channels_channel_participants* DS_CP = static_cast<tl_ds_channels_channel_participants*>(D);
        for (int i = 0; i < DS_LVAL(DS_CP->users->cnt); i++) {
            tglf_fetch_alloc_user(DS_CP->users->data[i]);
        }

        int count = DS_LVAL(DS_CP->participants->cnt);
        if (m_state->limit > 0) {
            int current_size = static_cast<int>(m_state->participants.size());
            assert(m_state->limit > current_size);
            count = std::min(count, m_state->limit - current_size);
        }
        for (int i = 0; i < count; i++) {
            bool admin = false;
            bool creator = false;
            auto magic = DS_CP->participants->data[i]->magic;
            if (magic == CODE_channel_participant_moderator || magic == CODE_channel_participant_editor) {
                admin = true;
            } else if (magic == CODE_channel_participant_creator) {
                creator = true;
                admin = true;
            }
            auto participant = std::make_shared<tgl_channel_participant>();
            participant->user_id = DS_LVAL(DS_CP->participants->data[i]->user_id);
            participant->inviter_id = DS_LVAL(DS_CP->participants->data[i]->inviter_id);
            participant->date = DS_LVAL(DS_CP->participants->data[i]->date);
            participant->is_creator = creator;
            participant->is_admin = admin;
            m_state->participants.push_back(participant);
        }
        m_state->offset += count;

        if (!count || (m_state->limit > 0 && static_cast<int>(m_state->participants.size()) == m_state->limit)) {
            if (m_state->participants.size()) {
                tgl_state::instance()->callback()->channel_update_participants(m_state->channel_id.peer_id, m_state->participants);
            }
            m_callback(true);
        } else {
            tgl_do_get_channel_participants(m_state, m_callback);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::shared_ptr<channel_get_participants_state> m_state;
    std::function<void(bool)> m_callback;
};

static void tgl_do_get_channel_participants(const std::shared_ptr<struct channel_get_participants_state>& state,
        const std::function<void(bool)>& callback)
{
    auto q = std::make_shared<query_channels_get_participants>(state, callback);
    q->out_i32(CODE_channels_get_participants);
    assert(state->channel_id.peer_type == tgl_peer_type::channel);
    q->out_i32(CODE_input_channel);
    q->out_i32(state->channel_id.peer_id);
    q->out_i64(state->channel_id.access_hash);

    switch (state->type) {
    case tgl_channel_participant_type::admins:
        q->out_i32(CODE_channel_participants_admins);
        break;
    case tgl_channel_participant_type::kicked:
        q->out_i32(CODE_channel_participants_kicked);
        break;
    case tgl_channel_participant_type::recent:
        q->out_i32(CODE_channel_participants_recent);
        break;
    case tgl_channel_participant_type::bots:
        q->out_i32(CODE_channel_participants_bots);
        break;
    }
    q->out_i32(state->offset);
    q->out_i32(state->limit);
    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_get_channel_participants(const tgl_input_peer_t& channel_id, int limit, int offset, tgl_channel_participant_type type,
        const std::function<void(bool success)>& callback)
{
    std::shared_ptr<channel_get_participants_state> state = std::make_shared<channel_get_participants_state>();
    state->type = type;
    state->channel_id = channel_id;
    state->limit = limit;
    state->offset = offset;
    tgl_do_get_channel_participants(state, callback);
}

/* {{{ Chat info */
class query_chat_info: public query
{
public:
    explicit query_chat_info(const std::function<void(bool)>& callback)
        : query("chat info", TYPE_TO_PARAM(messages_chat_full))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        std::shared_ptr<tgl_chat> chat = tglf_fetch_alloc_chat_full(static_cast<tl_ds_messages_chat_full*>(D));
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

void tgl_do_get_chat_info(int32_t id, const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_chat_info>(callback);
    q->out_i32(CODE_messages_get_full_chat);
    q->out_i32(id);
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ Channel info */
class query_channel_info: public query
{
public:
    explicit query_channel_info(const std::function<void(bool)>& callback)
        : query("channel info", TYPE_TO_PARAM(messages_chat_full))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        std::shared_ptr<tgl_channel> channel = tglf_fetch_alloc_channel_full(static_cast<tl_ds_messages_chat_full*>(D));
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

void tgl_do_get_channel_info(const tgl_input_peer_t& id,
        const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_channel_info>(callback);
    q->out_i32(CODE_channels_get_full_channel);
    assert(id.peer_type == tgl_peer_type::channel);
    q->out_i32(CODE_input_channel);
    q->out_i32(id.peer_id);
    q->out_i64(id.access_hash);
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ User info */
class query_user_info: public query
{
public:
    explicit query_user_info(const std::function<void(bool, const std::shared_ptr<tgl_user>&)>& callback)
        : query("user info", TYPE_TO_PARAM(user_full))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        std::shared_ptr<tgl_user> user = tglf_fetch_alloc_user_full(static_cast<tl_ds_user_full*>(D));
        if (m_callback) {
            m_callback(true, user);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false, nullptr);
        }
        return 0;
    }

private:
    std::function<void(bool, const std::shared_ptr<tgl_user>&)> m_callback;
};

void tgl_do_get_user_info(const tgl_input_peer_t& id, const std::function<void(bool success, const std::shared_ptr<tgl_user>& user)>& callback)
{
    if (id.peer_type != tgl_peer_type::user) {
        TGL_ERROR("id should be user id");
        if (callback) {
            callback(false, nullptr);
        }
        return;
    }

    auto q = std::make_shared<query_user_info>(callback);
    q->out_i32(CODE_users_get_full_user);
    assert(id.peer_type == tgl_peer_type::user);
    q->out_i32(CODE_input_user);
    q->out_i32(id.peer_id);
    q->out_i64(id.access_hash);
    q->execute(tgl_state::instance()->working_dc());
}

static void resend_query_cb(const std::shared_ptr<query>& q, bool success)
{
    assert(success);

    TGL_DEBUG("resend_query_cb");
    tgl_state::instance()->set_dc_logged_in(tgl_state::instance()->working_dc()->id);

    auto user_info_q = std::make_shared<query_user_info>(nullptr);
    user_info_q->out_i32(CODE_users_get_full_user);
    user_info_q->out_i32(CODE_input_user_self);
    user_info_q->execute(tgl_state::instance()->working_dc());

    if (auto dc = q->dc()) {
        dc->add_pending_query(q);
        dc->send_pending_queries();
    }
}
/* }}} */

/* {{{ Export auth */
class query_import_auth: public query
{
public:
    query_import_auth(const std::shared_ptr<tgl_dc>& dc,
            const std::function<void(bool)>& callback)
        : query("import authorization", TYPE_TO_PARAM(auth_authorization))
        , m_dc(dc)
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_auth_authorization* DS_U = static_cast<tl_ds_auth_authorization*>(D);
        tglf_fetch_alloc_user(DS_U->user);

        assert(m_dc);
        TGL_NOTICE("auth imported from DC " << tgl_state::instance()->working_dc()->id << " to DC " << m_dc->id);

        tgl_state::instance()->set_dc_logged_in(m_dc->id);

        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::shared_ptr<tgl_dc> m_dc;
    std::function<void(bool)> m_callback;
};

class query_export_auth: public query
{
public:
    query_export_auth(const std::shared_ptr<tgl_dc>& dc,
            const std::function<void(bool)>& callback)
        : query("export authorization", TYPE_TO_PARAM(auth_exported_authorization))
        , m_dc(dc)
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        TGL_NOTICE("export_auth_on_answer " << m_dc->id);
        tl_ds_auth_exported_authorization* DS_EA = static_cast<tl_ds_auth_exported_authorization*>(D);
        tgl_state::instance()->set_our_id(DS_LVAL(DS_EA->id));

        auto q = std::make_shared<query_import_auth>(m_dc, m_callback);
        q->out_header();
        q->out_i32(CODE_auth_import_authorization);
        q->out_i32(tgl_state::instance()->our_id().peer_id);
        q->out_string(DS_STR(DS_EA->bytes));
        q->execute(m_dc, query::execution_option::LOGIN);
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::shared_ptr<tgl_dc> m_dc;
    std::function<void(bool)> m_callback;
};

// export auth from working DC and import to DC "num"
static void tgl_do_transfer_auth(const std::shared_ptr<tgl_dc>& dc, const std::function<void(bool success)>& callback)
{
    if (dc->auth_transfer_in_process) {
        return;
    }
    dc->auth_transfer_in_process = true;
    TGL_NOTICE("transferring auth from DC " << tgl_state::instance()->working_dc()->id << " to DC " << dc->id);
    auto q = std::make_shared<query_export_auth>(dc, callback);
    q->out_i32(CODE_auth_export_authorization);
    q->out_i32(dc->id);
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ Add contact */
class query_add_contacts: public query
{
public:
    explicit query_add_contacts(const std::function<void(bool, const std::vector<int32_t>&)>& callback)
        : query("add contacts", TYPE_TO_PARAM(contacts_imported_contacts))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_contacts_imported_contacts* DS_CIC = static_cast<tl_ds_contacts_imported_contacts*>(D);
        TGL_DEBUG(DS_LVAL(DS_CIC->imported->cnt) << " contact(s) added");
        int n = DS_LVAL(DS_CIC->users->cnt);
        std::vector<int> users(n);
        for (int i = 0; i < n; i++) {
            users[i] = tglf_fetch_alloc_user(DS_CIC->users->data[i])->id.peer_id;
        }
        if (m_callback) {
            m_callback(true, users);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false, std::vector<int>());
        }
        return 0;
    }

private:
    std::function<void(bool, const std::vector<int>&)> m_callback;
};

void tgl_do_add_contacts(const std::vector<std::tuple<std::string, std::string, std::string>>& contacts, bool replace,
        const std::function<void(bool success, const std::vector<int32_t>& user_ids)>& callback)
{
    auto q = std::make_shared<query_add_contacts>(callback);
    q->out_i32(CODE_contacts_import_contacts);
    q->out_i32(CODE_vector);
    q->out_i32(contacts.size());
    int64_t r;

    for (const auto& contact : contacts) {
        const auto& phone = std::get<0>(contact);
        const auto& first_name = std::get<1>(contact);
        const auto& last_name = std::get<2>(contact);

        q->out_i32(CODE_input_phone_contact);
        tgl_secure_random(reinterpret_cast<unsigned char*>(&r), 8);
        q->out_i64(r);
        q->out_std_string(phone);
        q->out_std_string(first_name);
        q->out_std_string(last_name);
    }

    q->out_i32(replace ? CODE_bool_true : CODE_bool_false);
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ Del contact */
class query_delete_contact: public query
{
public:
    query_delete_contact(int32_t user_id, const std::function<void(bool)>& callback)
        : query("delete contact", TYPE_TO_PARAM(contacts_link))
        , m_user_id(user_id)
        , m_callback(callback)
    { }

    virtual void on_answer(void*) override
    {
        tgl_state::instance()->callback()->user_deleted(m_user_id);
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    int32_t m_user_id;
    std::function<void(bool)> m_callback;
};

void tgl_do_delete_contact(const tgl_input_peer_t& id, const std::function<void(bool success)>& callback)
{
    if (id.peer_type != tgl_peer_type::user) {
        TGL_ERROR("the peer id user be user id");
        if (callback) {
            callback(false);
        }
        return;
    }

    auto q = std::make_shared<query_delete_contact>(id.peer_id, callback);
    q->out_i32(CODE_contacts_delete_contact);
    q->out_i32(CODE_input_user);
    q->out_i32(id.peer_id);
    q->out_i64(id.access_hash);
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ Msg search */

struct msg_search_state {
    msg_search_state(const tgl_input_peer_t& id, int from, int to, int limit, int offset, const std::string &query) :
        id(id), from(from), to(to), limit(limit), offset(offset), query(query) {}
    std::vector<std::shared_ptr<tgl_message>> messages;
    tgl_input_peer_t id;
    int from;
    int to;
    int limit;
    int offset;
    int max_id = 0;
    std::string query;
};

static void tgl_do_msg_search(const std::shared_ptr<msg_search_state>& state,
        const std::function<void(bool, const std::vector<std::shared_ptr<tgl_message>>&)>& callback);

class query_msg_search: public query
{
public:
    query_msg_search(const std::shared_ptr<msg_search_state>& state,
            const std::function<void(bool, const std::vector<std::shared_ptr<tgl_message>>&)>& callback)
        : query("messages search", TYPE_TO_PARAM(messages_messages))
        , m_state(state)
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_messages_messages* DS_MM = static_cast<tl_ds_messages_messages*>(D);
        for (int i = 0; i < DS_LVAL(DS_MM->chats->cnt); i++) {
            tglf_fetch_alloc_chat(DS_MM->chats->data[i]);
        }
        for (int i = 0; i < DS_LVAL(DS_MM->users->cnt); i++) {
            tglf_fetch_alloc_user(DS_MM->users->data[i]);
        }

        int n = DS_LVAL(DS_MM->messages->cnt);
        for (int i = 0; i < n; i++) {
            m_state->messages.push_back(tglf_fetch_alloc_message(DS_MM->messages->data[i]));
        }
        tgl_state::instance()->callback()->new_messages(m_state->messages);
        m_state->offset += n;
        m_state->limit -= n;
        if (m_state->limit + m_state->offset >= DS_LVAL(DS_MM->count)) {
            m_state->limit = DS_LVAL(DS_MM->count) - m_state->offset;
            if (m_state->limit < 0) {
                m_state->limit = 0;
            }
        }
        assert(m_state->limit >= 0);

        if (m_state->limit <= 0 || DS_MM->magic == CODE_messages_messages) {
            if (m_callback) {
                m_callback(true, m_state->messages);
            }
        } else {
            m_state->max_id = m_state->messages[m_state->messages.size()-1]->permanent_id;
            m_state->offset = 0;
            tgl_do_msg_search(m_state, m_callback);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " <<  error_code << " " << error_string);
        if (m_callback) {
            m_callback(0, std::vector<std::shared_ptr<tgl_message>>());
        }
        return 0;
    }

private:
    std::shared_ptr<msg_search_state> m_state;
    std::function<void(bool, const std::vector<std::shared_ptr<tgl_message>>&)> m_callback;
};

static void tgl_do_msg_search(const std::shared_ptr<msg_search_state>& state,
        const std::function<void(bool, const std::vector<std::shared_ptr<tgl_message>>&)>& callback)
{
    auto q = std::make_shared<query_msg_search>(state, callback);
    if (state->id.peer_type == tgl_peer_type::unknown) {
        q->out_i32(CODE_messages_search_global);
        q->out_string(state->query.c_str());
        q->out_i32(0);
        q->out_i32(CODE_input_peer_empty);
        q->out_i32(state->offset);
        q->out_i32(state->limit);
    } else {
        q->out_i32(CODE_messages_search);
        q->out_i32(0);
        q->out_input_peer(state->id);
        q->out_string(state->query.c_str());
        q->out_i32(CODE_input_messages_filter_empty);
        q->out_i32(state->from);
        q->out_i32(state->to);
        q->out_i32(state->offset); // offset
        q->out_i32(state->max_id); // max_id
        q->out_i32(state->limit);
    }
    q->execute(tgl_state::instance()->working_dc());
}

//untested
void tgl_do_msg_search(const tgl_input_peer_t& id, int from, int to, int limit, int offset, const std::string &query,
        const std::function<void(bool success, const std::vector<std::shared_ptr<tgl_message>>& list)>& callback) {
    if (id.peer_type == tgl_peer_type::enc_chat) {
        TGL_ERROR("can not search in secret chats");
        if (callback) {
            callback(false, std::vector<std::shared_ptr<tgl_message>>());
        }
        return;
    }
    std::shared_ptr<msg_search_state> state = std::make_shared<msg_search_state>(id, from, to, limit, offset, query);
    tgl_do_msg_search(state, callback);
}
/* }}} */

/* {{{ Get difference */
class query_get_state: public query
{
public:
    query_get_state(const std::function<void(bool)>& callback)
        : query("get state", TYPE_TO_PARAM(updates_state))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_updates_state* DS_US = static_cast<tl_ds_updates_state*>(D);
        assert(tgl_state::instance()->is_diff_locked());
        tgl_state::instance()->set_diff_locked(false);
        tgl_state::instance()->set_pts(DS_LVAL(DS_US->pts));
        tgl_state::instance()->set_qts(DS_LVAL(DS_US->qts));
        tgl_state::instance()->set_date(DS_LVAL(DS_US->date));
        tgl_state::instance()->set_seq(DS_LVAL(DS_US->seq));
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

class query_lookup_state: public query
{
public:
    explicit query_lookup_state(const std::function<void(bool)>& callback)
        : query("lookup state", TYPE_TO_PARAM(updates_state))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_updates_state* DS_US = static_cast<tl_ds_updates_state*>(D);
        int pts = DS_LVAL(DS_US->pts);
        int qts = DS_LVAL(DS_US->qts);
        int seq = DS_LVAL(DS_US->seq);
        if (pts > tgl_state::instance()->pts() || qts > tgl_state::instance()->qts() || seq > tgl_state::instance()->seq()) {
            tgl_do_get_difference(false, nullptr);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

class query_get_difference: public query
{
public:
    query_get_difference(const std::function<void(bool)>& callback)
        : query("get difference", TYPE_TO_PARAM(updates_difference))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        TGL_DEBUG("get difference answer");

        tl_ds_updates_difference* DS_UD = static_cast<tl_ds_updates_difference*>(D);

        assert(tgl_state::instance()->is_diff_locked());
        tgl_state::instance()->set_diff_locked(false);

        if (DS_UD->magic == CODE_updates_difference_empty) {
            tgl_state::instance()->set_date(DS_LVAL(DS_UD->date));
            tgl_state::instance()->set_seq(DS_LVAL(DS_UD->seq));

            TGL_DEBUG("empty difference, seq = " << tgl_state::instance()->seq());
            if (m_callback) {
                m_callback(true);
            }
        } else {
            for (int i = 0; i < DS_LVAL(DS_UD->users->cnt); i++) {
                tglf_fetch_alloc_user(DS_UD->users->data[i]);
            }
            for (int i = 0; i < DS_LVAL(DS_UD->chats->cnt); i++) {
                tglf_fetch_alloc_chat(DS_UD->chats->data[i]);
            }

            int message_count = DS_LVAL(DS_UD->new_messages->cnt);
            std::vector<std::shared_ptr<tgl_message>> messages;
            for (int i = 0; i < message_count; i++) {
                messages.push_back(tglf_fetch_alloc_message(DS_UD->new_messages->data[i]));
            }
            tgl_state::instance()->callback()->new_messages(messages);

            int encrypted_message_count = DS_LVAL(DS_UD->new_encrypted_messages->cnt);
            std::vector<std::shared_ptr<tgl_secret_message>> secret_messages;
            for (int i = 0; i < encrypted_message_count; i++) {
                if (auto secret_message = tglf_fetch_encrypted_message(DS_UD->new_encrypted_messages->data[i])) {
                    TGL_DEBUG("received secret message, layer = " << secret_message->layer
                            << ", in_seq_no = " << secret_message->in_seq_no
                            << ", out_seq_no = " << secret_message->out_seq_no);
                    secret_messages.push_back(secret_message);
                }
            }
            std::stable_sort(secret_messages.begin(), secret_messages.end(),
                    [&](const std::shared_ptr<tgl_secret_message>& a, const std::shared_ptr<tgl_secret_message>& b) {
                        return a->out_seq_no < b->out_seq_no;
                    });
            for (const auto& secret_message: secret_messages) {
                    TGL_DEBUG("received secret message after sorting, layer = " << secret_message->layer
                            << ", in_seq_no = " << secret_message->in_seq_no
                            << ", out_seq_no = " << secret_message->out_seq_no);
                tglf_encrypted_message_received(secret_message);
            }

            for (int i = 0; i < DS_LVAL(DS_UD->other_updates->cnt); i++) {
                tglu_work_update(DS_UD->other_updates->data[i], nullptr, tgl_update_mode::dont_check_and_update_consistency);
            }
#if 0
            for (int i = 0; i < message_count; i++) {
                bl_do_msg_update(&messages[i]->permanent_id);
                tgl_state::instance()->callback()->new_message(messages[i]);
            }
#endif

            if (DS_UD->state) {
                tgl_state::instance()->set_pts(DS_LVAL(DS_UD->state->pts));
                tgl_state::instance()->set_qts(DS_LVAL(DS_UD->state->qts));
                tgl_state::instance()->set_date(DS_LVAL(DS_UD->state->date));
                tgl_state::instance()->set_seq(DS_LVAL(DS_UD->state->seq));
                if (m_callback) {
                    m_callback(true);
                }
            } else {
                tgl_state::instance()->set_pts(DS_LVAL(DS_UD->intermediate_state->pts));
                tgl_state::instance()->set_qts(DS_LVAL(DS_UD->intermediate_state->qts));
                tgl_state::instance()->set_date(DS_LVAL(DS_UD->intermediate_state->date));
                tgl_do_get_difference(false, m_callback);
            }
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

void tgl_do_lookup_state()
{
    if (tgl_state::instance()->is_diff_locked()) {
        return;
    }
    auto q = std::make_shared<query_lookup_state>(nullptr);
    q->out_header();
    q->out_i32(CODE_updates_get_state);
    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_get_difference(bool sync_from_start, const std::function<void(bool success)>& callback)
{
    if (tgl_state::instance()->is_diff_locked()) {
        if (callback) {
            callback(false);
        }
        return;
    }
    tgl_state::instance()->set_diff_locked(true);
    if (tgl_state::instance()->pts() > 0 || sync_from_start) {
        if (tgl_state::instance()->pts() == 0) {
            tgl_state::instance()->set_pts(1, true);
        }
        if (tgl_state::instance()->date() == 0) {
            tgl_state::instance()->set_date(1, true);
        }
        auto q = std::make_shared<query_get_difference>(callback);
        q->out_header();
        q->out_i32(CODE_updates_get_difference);
        q->out_i32(tgl_state::instance()->pts());
        q->out_i32(tgl_state::instance()->date());
        q->out_i32(tgl_state::instance()->qts());
        q->execute(tgl_state::instance()->working_dc());
    } else {
        auto q = std::make_shared<query_get_state>(callback);
        q->out_header();
        q->out_i32(CODE_updates_get_state);
        q->execute(tgl_state::instance()->working_dc());
    }
}
/* }}} */

/* {{{ Get channel difference */
class query_get_channel_difference: public query
{
public:
    query_get_channel_difference(const std::shared_ptr<tgl_channel>& channel,
            const std::function<void(bool)>& callback)
        : query("get channel difference", TYPE_TO_PARAM(updates_channel_difference))
        , m_channel(channel)
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_updates_channel_difference* DS_UD = static_cast<tl_ds_updates_channel_difference*>(D);

        assert(m_channel->diff_locked);
        m_channel->diff_locked = false;

        if (DS_UD->magic == CODE_updates_channel_difference_empty) {
            //bl_do_set_channel_pts(tgl_get_peer_id(channel->id), DS_LVAL(DS_UD->channel_pts));
            TGL_DEBUG("empty difference, seq = " << tgl_state::instance()->seq());
            if (m_callback) {
                m_callback(true);
            }
        } else {
            for (int i = 0; i < DS_LVAL(DS_UD->users->cnt); i++) {
                tglf_fetch_alloc_user(DS_UD->users->data[i]);
            }

            for (int i = 0; i < DS_LVAL(DS_UD->chats->cnt); i++) {
                tglf_fetch_alloc_chat(DS_UD->chats->data[i]);
            }

            int message_count = DS_LVAL(DS_UD->new_messages->cnt);
            std::vector<std::shared_ptr<tgl_message>> messages;
            for (int i = 0; i < message_count; i++) {
                messages.push_back(tglf_fetch_alloc_message(DS_UD->new_messages->data[i]));
            }
            tgl_state::instance()->callback()->new_messages(messages);

            for (int i = 0; i < DS_LVAL(DS_UD->other_updates->cnt); i++) {
                tglu_work_update(DS_UD->other_updates->data[i], nullptr, tgl_update_mode::dont_check_and_update_consistency);
            }

#if 0
            for (int i = 0; i < ml_pos; i++) {
                bl_do_msg_update(&messages[i]->permanent_id);
            }
#endif

            //bl_do_set_channel_pts(tgl_get_peer_id(m_channel->id), DS_LVAL(DS_UD->channel_pts));
            if (DS_UD->magic != CODE_updates_channel_difference_too_long) {
                if (m_callback) {
                    m_callback(true);
                }
            } else {
                tgl_do_get_channel_difference(m_channel->id, m_callback);
            }
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::shared_ptr<tgl_channel> m_channel;
    std::function<void(bool)> m_callback;
};

void tgl_do_get_channel_difference(const tgl_input_peer_t& channel_id, const std::function<void(bool success)>& callback)
{
    std::shared_ptr<struct tgl_channel> channel = std::make_shared<struct tgl_channel>();
    channel->id = channel_id;

    if (!channel->pts) {
        if (callback) {
            callback(false);
        }
        return;
    }
    //get_difference_active = 1;
    //difference_got = 0;
    if (channel->diff_locked) {
        TGL_WARNING("channel " << channel->id.peer_id << " diff locked");
        if (callback) {
            callback(false);
        }
        return;
    }
    channel->diff_locked = true;

    auto q = std::make_shared<query_get_channel_difference>(channel, callback);
    q->out_header();
    q->out_i32(CODE_updates_get_channel_difference);
    q->out_i32(CODE_input_channel);
    q->out_i32(channel->id.peer_id);
    q->out_i64(channel->id.access_hash);
    q->out_i32(CODE_channel_messages_filter_empty);
    q->out_i32(channel->pts);
    q->out_i32(100);
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ Visualize key */

//int tgl_do_visualize_key(tgl_peer_id_t id, unsigned char buf[16]) {
//    assert(id.peer_type == tgl_peer_type::enc_chat);
//    assert(P);
//    if (P->encr_chat.state != tgl_secret_chat_state::ok) {
//        TGL_WARNING("Chat is not initialized yet");
//        return -1;
//    }
//    memcpy(buf, P->encr_chat.first_key_sha, 16);
//    return 0;
//}
/* }}} */

void tgl_do_add_user_to_chat(const tgl_peer_id_t& chat_id, const tgl_input_peer_t& user_id, int32_t limit,
        const std::function<void(bool success)>& callback) {
    auto q = std::make_shared<query_send_msgs>(callback);
    q->out_i32(CODE_messages_add_chat_user);
    q->out_i32(chat_id.peer_id);

    assert(user_id.peer_type == tgl_peer_type::user);
    q->out_i32(CODE_input_user);
    q->out_i32(user_id.peer_id);
    q->out_i64(user_id.access_hash);
    q->out_i32(limit);

    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_delete_user_from_chat(int32_t chat_id, const tgl_input_peer_t& user_id,
        const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_send_msgs>(callback);
    q->out_i32(CODE_messages_delete_chat_user);
    q->out_i32(chat_id);

    assert(user_id.peer_type == tgl_peer_type::user);
    if (user_id.peer_id == tgl_state::instance()->our_id().peer_id) {
        q->out_i32(CODE_input_user_self);
    } else {
        q->out_i32(CODE_input_user);
        q->out_i32(user_id.peer_id);
        q->out_i64(user_id.access_hash);
    }

    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_channel_invite_user(const tgl_input_peer_t& channel_id, const std::vector<tgl_input_peer_t>& user_ids,
        const std::function<void(bool success)>& callback)
{
    if (user_ids.empty()) {
        if (callback) {
            callback(true);
        }
        return;
    }

    auto q = std::make_shared<query_send_msgs>(callback);
    q->out_i32(CODE_channels_invite_to_channel);
    q->out_i32(CODE_input_channel);
    q->out_i32(channel_id.peer_id);
    q->out_i64(channel_id.access_hash);

    q->out_i32(CODE_vector);
    q->out_i32(user_ids.size());
    for (const auto& user_id: user_ids) {
        assert(user_id.peer_type == tgl_peer_type::user);
        q->out_i32(CODE_input_user);
        q->out_i32(user_id.peer_id);
        q->out_i64(user_id.access_hash);
    }

    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_channel_delete_user(const tgl_input_peer_t& channel_id, const tgl_input_peer_t& user_id,
    const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_send_msgs>(callback);
    q->out_i32(CODE_channels_kick_from_channel);
    q->out_i32(CODE_input_channel);
    q->out_i32(channel_id.peer_id);
    q->out_i64(channel_id.access_hash);

    q->out_i32(CODE_input_user);
    q->out_i32(user_id.peer_id);
    q->out_i64(user_id.access_hash);

    q->out_i32(CODE_bool_true);

    q->execute(tgl_state::instance()->working_dc());
}

class query_create_chat: public query
{
public:
    explicit query_create_chat(const std::function<void(int32_t chat_id)>& callback, bool is_channel = false)
        : query(is_channel ? "create channel" : "create chat", TYPE_TO_PARAM(updates))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_updates* DS_U = static_cast<tl_ds_updates*>(D);
        tglu_work_any_updates(DS_U, nullptr);

        int32_t chat_id = 0;
        if (DS_U->magic == CODE_updates && DS_U->chats && DS_U->chats->cnt && *DS_U->chats->cnt == 1) {
            chat_id = DS_LVAL(DS_U->chats->data[0]->id);
        }

        if (!chat_id) {
            assert(false);
        }

        if (m_callback) {
            m_callback(chat_id);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(0);
        }
        return 0;
    }

private:
    std::function<void(int32_t)> m_callback;
};

void tgl_do_create_group_chat(const std::vector<tgl_input_peer_t>& user_ids, const std::string& chat_topic,
        const std::function<void(int32_t chat_id)>& callback)
{
    auto q = std::make_shared<query_create_chat>(callback);
    q->out_i32(CODE_messages_create_chat);
    q->out_i32(CODE_vector);
    q->out_i32(user_ids.size()); // Number of users, currently we support only 1 user.
    for (auto id : user_ids) {
        if (id.peer_type != tgl_peer_type::user) {
            TGL_ERROR("can not create chat with unknown user");
            if (callback) {
                callback(false);
            }
            return;
        }
        q->out_i32(CODE_input_user);
        q->out_i32(id.peer_id);
        q->out_i64(id.access_hash);
    }
    TGL_DEBUG("sending out chat creat request users number: " << user_ids.size());
    q->out_string(chat_topic.c_str(), chat_topic.length());
    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_create_channel(const std::string& topic, const std::string& about,
        bool broadcast, bool mega_group,
        const std::function<void(int32_t channel_id)>& callback)
{
    int32_t flags = 0;
    if (broadcast) {
        flags |= 1;
    }
    if (mega_group) {
        flags |= 2;
    }
    auto q = std::make_shared<query_create_chat>(callback, true);
    q->out_i32(CODE_channels_create_channel);
    q->out_i32(flags);
    q->out_std_string(topic);
    q->out_std_string(about);

    q->execute(tgl_state::instance()->working_dc());
}

/* {{{ Delete msg */
class query_delete_msg: public query
{
public:
    query_delete_msg(const tgl_input_peer_t& chat,
            const std::function<void(bool)>& callback)
        : query("delete message", TYPE_TO_PARAM(messages_affected_messages))
        , m_chat(chat)
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_messages_affected_messages* DS_MAM = static_cast<tl_ds_messages_affected_messages*>(D);
#if 0 // FIXME
        struct tgl_message* M = tgl_message_get(id.get());
        if (M) {
            bl_do_message_delete(&M->permanent_id);
        }
#endif
        tgl_state::instance()->callback()->message_deleted(m_chat.peer_id);

        if (tgl_check_pts_diff(DS_LVAL(DS_MAM->pts), DS_LVAL(DS_MAM->pts_count))) {
            tgl_state::instance()->set_pts(DS_LVAL(DS_MAM->pts));
        }

        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    tgl_input_peer_t m_chat;
    std::function<void(bool)> m_callback;
};

void tgl_do_delete_msg(const tgl_input_peer_t& chat, int64_t message_id,
        const std::function<void(bool success)>& callback)
{
    if (chat.peer_type == tgl_peer_type::enc_chat) {
        std::shared_ptr<tgl_secret_chat> secret_chat = tgl_state::instance()->secret_chat_for_id(chat.peer_id);
        if (!secret_chat) {
            TGL_ERROR("could not find secret chat");
            return;
        }
        tgl_do_messages_delete_encr(secret_chat, message_id, nullptr);
        if (callback) {
            callback(false);
        }
        return;
    }

    if (chat.peer_type == tgl_peer_type::temp_id) {
        TGL_ERROR("unknown message");
        if (callback) {
            callback(false);
        }
        return;
    }
    auto q = std::make_shared<query_delete_msg>(chat, callback);
    if (chat.peer_type == tgl_peer_type::channel) {
        q->out_i32(CODE_channels_delete_messages);
        q->out_i32(CODE_input_channel);
        q->out_i32(chat.peer_id);
        q->out_i64(chat.access_hash);

        q->out_i32(CODE_vector);
        q->out_i32(1);
        q->out_i32(message_id);
    } else {
        q->out_i32(CODE_messages_delete_messages);
        q->out_i32(CODE_vector);
        q->out_i32(1);
        q->out_i32(message_id);
    }

    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ Export card */
static constexpr struct paramed_type bare_int_type = TYPE_TO_PARAM(bare_int);
static constexpr struct paramed_type bare_int_array_type[1] = {bare_int_type};
static constexpr struct paramed_type vector_type = (struct paramed_type) {.type = tl_type_vector, .params=bare_int_array_type};

class query_export_card: public query
{
public:
    explicit query_export_card(const std::function<void(bool, const std::vector<int>&)>& callback)
        : query("export card", vector_type)
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_vector* DS_V = static_cast<tl_ds_vector*>(D);
        int n = DS_LVAL(DS_V->f1);
        std::vector<int> card;
        for (int i = 0; i < n; i++) {
            card.push_back(*reinterpret_cast<int*>(DS_V->f2[i]));
        }
        if (m_callback) {
            m_callback(true, card);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false, std::vector<int>());
        }
        return 0;
    }

private:
    std::function<void(bool, const std::vector<int>&)> m_callback;
};

void tgl_do_export_card(const std::function<void(bool success, const std::vector<int>& card)>& callback)
{
    auto q = std::make_shared<query_export_card>(callback);
    q->out_i32(CODE_contacts_export_card);
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ Import card */
class query_import_card: public query
{
public:
    explicit query_import_card(const std::function<void(bool, const std::shared_ptr<tgl_user>&)>& callback)
        : query("import card", TYPE_TO_PARAM(user))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        std::shared_ptr<tgl_user> user = tglf_fetch_alloc_user(static_cast<tl_ds_user*>(D));
        if (m_callback) {
            m_callback(true, user);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false, nullptr);
        }
        return 0;
    }

private:
    std::function<void(bool, const std::shared_ptr<tgl_user>&)> m_callback;
};

void tgl_do_import_card(int size, int* card,
        const std::function<void(bool success, const std::shared_ptr<tgl_user>& user)>& callback)
{
    auto q = std::make_shared<query_import_card>(callback);
    q->out_i32(CODE_contacts_import_card);
    q->out_i32(CODE_vector);
    q->out_i32(size);
    q->out_i32s(card, size);
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

void tgl_do_start_bot(const tgl_input_peer_t& bot, const tgl_peer_id_t& chat,
        const char* str, int str_len,
        const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_send_msgs>(callback);
    q->out_i32(CODE_messages_start_bot);
    q->out_i32(CODE_input_user);
    q->out_i32(bot.peer_id);
    q->out_i64(bot.access_hash);
    q->out_i32(chat.peer_id);
    int64_t m;
    tgl_secure_random(reinterpret_cast<unsigned char*>(&m), 8);
    q->out_i64(m);
    q->out_string(str, str_len);

    q->execute(tgl_state::instance()->working_dc());
}

/* {{{ Send typing */
class query_send_typing: public query
{
public:
    explicit query_send_typing(const std::function<void(bool)>& callback)
        : query("send typing", TYPE_TO_PARAM(bool))
        , m_callback(callback)
    { }

    virtual void on_answer(void*) override
    {
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

    virtual double timeout_interval() const override
    {
        return 120;
    }

private:
    std::function<void(bool)> m_callback;
};

void tgl_do_send_typing(const tgl_input_peer_t& id, tgl_typing_status status,
        const std::function<void(bool success)>& callback)
{
    if (id.peer_type != tgl_peer_type::enc_chat) {
        auto q = std::make_shared<query_send_typing>(callback);
        q->out_i32(CODE_messages_set_typing);
        q->out_input_peer(id);
        switch (status) {
        case tgl_typing_status::none:
        case tgl_typing_status::typing:
            q->out_i32(CODE_send_message_typing_action);
            break;
        case tgl_typing_status::cancel:
            q->out_i32(CODE_send_message_cancel_action);
            break;
        case tgl_typing_status::record_video:
            q->out_i32(CODE_send_message_record_video_action);
            break;
        case tgl_typing_status::upload_video:
            q->out_i32(CODE_send_message_upload_video_action);
            q->out_i32(0);
            break;
        case tgl_typing_status::record_audio:
            q->out_i32(CODE_send_message_record_audio_action);
            break;
        case tgl_typing_status::upload_audio:
            q->out_i32(CODE_send_message_upload_audio_action);
            q->out_i32(0);
            break;
        case tgl_typing_status::upload_photo:
            q->out_i32(CODE_send_message_upload_photo_action);
            q->out_i32(0);
            break;
        case tgl_typing_status::upload_document:
            q->out_i32(CODE_send_message_upload_document_action);
            q->out_i32(0);
            break;
        case tgl_typing_status::geo:
            q->out_i32(CODE_send_message_geo_location_action);
            break;
        case tgl_typing_status::choose_contact:
            q->out_i32(CODE_send_message_choose_contact_action);
            break;
        }
        q->execute(tgl_state::instance()->working_dc());
    } else {
        if (callback) {
            callback(false);
        }
    }
}
/* }}} */

/* {{{ get messages */
class query_get_messages: public query
{
public:
    explicit query_get_messages(const std::function<void(bool, const std::shared_ptr<tgl_message>&)>& single_callback)
        : query("get messages (single)", TYPE_TO_PARAM(messages_messages))
        , m_single_callback(single_callback)
        , m_multi_callback(nullptr)
    { }

    explicit query_get_messages(const std::function<void(bool, const std::vector<std::shared_ptr<tgl_message>>&)>& multi_callback)
        : query("get messages (multi)", TYPE_TO_PARAM(messages_messages))
        , m_single_callback(nullptr)
        , m_multi_callback(multi_callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_messages_messages* DS_MM = static_cast<tl_ds_messages_messages*>(D);
        for (int i = 0; i < DS_LVAL(DS_MM->users->cnt); i++) {
            tglf_fetch_alloc_user(DS_MM->users->data[i]);
        }
        for (int i = 0; i < DS_LVAL(DS_MM->chats->cnt); i++) {
            tglf_fetch_alloc_chat(DS_MM->chats->data[i]);
        }

        std::vector<std::shared_ptr<tgl_message>> messages;
        for (int i = 0; i < DS_LVAL(DS_MM->messages->cnt); i++) {
            messages.push_back(tglf_fetch_alloc_message(DS_MM->messages->data[i]));
        }
        tgl_state::instance()->callback()->new_messages(messages);
        if (m_multi_callback) {
            assert(!m_single_callback);
            m_multi_callback(true, messages);
        } else if (m_single_callback) {
            assert(!m_multi_callback);
            if (messages.size() > 0) {
                m_single_callback(true, messages[0]);
            } else {
                TGL_ERROR("no such message");
                m_single_callback(false, nullptr);
            }
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_multi_callback) {
            assert(!m_single_callback);
            m_multi_callback(false, std::vector<std::shared_ptr<tgl_message>>());
        } else if (m_single_callback) {
            assert(!m_multi_callback);
            m_single_callback(false, nullptr);
        }
        return 0;
    }

private:
    std::function<void(bool, const std::shared_ptr<tgl_message>&)> m_single_callback;
    std::function<void(bool, const std::vector<std::shared_ptr<tgl_message>>&)> m_multi_callback;
};

void tgl_do_get_message(int64_t message_id, const std::function<void(bool success, const std::shared_ptr<tgl_message>& M)>& callback)
{
#if 0
    struct tgl_message* M = tgl_message_get(&msg_id);
    if (M) {
        if (callback) {
            callback(true, M);
        }
        return;
    }
#endif

    auto q = std::make_shared<query_get_messages>(callback);

    q->out_i32(CODE_messages_get_messages);
    q->out_i32(CODE_vector);
    q->out_i32(1);
    q->out_i32(message_id);

    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ Export/import chat link */
class query_export_chat_link: public query
{
public:
    explicit query_export_chat_link(const std::function<void(bool, const std::string&)>& callback)
        : query("export chat link", TYPE_TO_PARAM(exported_chat_invite))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_exported_chat_invite* DS_ECI = static_cast<tl_ds_exported_chat_invite*>(D);
        if (m_callback) {
            std::string link;
            if (DS_ECI->link && DS_ECI->link->data) {
                link = std::string(DS_ECI->link->data, DS_ECI->link->len);
            }
            m_callback(true, link);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false, std::string());
        }
        return 0;
    }

private:
    std::function<void(bool, const std::string&)> m_callback;
};

void tgl_do_export_chat_link(const tgl_peer_id_t& id, const std::function<void(bool success, const std::string& link)>& callback)
{
    if (id.peer_type != tgl_peer_type::chat) {
        TGL_ERROR("Can only export chat link for chat");
        if (callback) {
            callback(false, std::string());
        }
        return;
    }

    auto q = std::make_shared<query_export_chat_link>(callback);
    q->out_i32(CODE_messages_export_chat_invite);
    q->out_i32(id.peer_id);

    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_import_chat_link(const std::string& link,
        const std::function<void(bool success)> callback)
{
    const char* link_str = link.c_str();
    const char* l = link_str + link.size() - 1;
    while (l >= link_str && *l != '/') {
        l--;
    }
    l++;

    auto q = std::make_shared<query_send_msgs>(callback);
    q->out_i32(CODE_messages_import_chat_invite);
    q->out_string(l, link.size() - (l - link_str));

    q->execute(tgl_state::instance()->working_dc());
}

/* }}} */

/* {{{ Export/import channel link */

void tgl_do_export_channel_link(const tgl_input_peer_t& id, const std::function<void(bool success, const std::string& link)>& callback)
{
    if (id.peer_type != tgl_peer_type::channel) {
        TGL_ERROR("can only export chat link for chat");
        if (callback) {
            callback(false, std::string());
        }
        return;
    }

    auto q = std::make_shared<query_export_chat_link>(callback);
    q->out_i32(CODE_channels_export_invite);
    q->out_i32(CODE_input_channel);
    q->out_i32(id.peer_id);
    q->out_i64(id.access_hash);

    q->execute(tgl_state::instance()->working_dc());
}

/* }}} */

/* {{{ set password */
class query_set_password: public query
{
public:
    explicit query_set_password(const std::function<void(bool)>& callback)
        : query("set password", TYPE_TO_PARAM(bool))
        , m_callback(callback)
    { }

    virtual void on_answer(void*) override
    {
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        if (error_code == 400) {
            if (error_string == "PASSWORD_HASH_INVALID") {
                TGL_WARNING("bad old password");
                if (m_callback) {
                    m_callback(false);
                }
                return 0;
            }
            if (error_string == "NEW_PASSWORD_BAD") {
                TGL_WARNING("bad new password (unchanged or equals hint)");
                if (m_callback) {
                    m_callback(false);
                }
                return 0;
            }
            if (error_string == "NEW_SALT_INVALID") {
                TGL_WARNING("bad new salt");
                if (m_callback) {
                    m_callback(false);
                }
                return 0;
            }
        }

        TGL_ERROR("RPC_CALL_FAIL " <<  error_code << " " << error_string);

        if (m_callback) {
            m_callback(false);
        }

        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

static void tgl_do_act_set_password(const std::string& current_password,
        const std::string& new_password,
        const std::string& current_salt,
        const std::string& new_salt,
        const std::string& hint,
        const std::function<void(bool success)>& callback)
{
    char s[512];
    unsigned char shab[32];
    memset(s, 0, sizeof(s));
    memset(shab, 0, sizeof(shab));

    if (current_salt.size() > 128 || current_password.size() > 128 || new_salt.size() > 128 || new_password.size() > 128) {
        if (callback) {
            callback(false);
        }
        return;
    }

    auto q = std::make_shared<query_set_password>(callback);
    q->out_i32(CODE_account_update_password_settings);

    if (current_password.size() && current_salt.size()) {
        memcpy(s, current_salt.data(), current_salt.size());
        memcpy(s + current_salt.size(), current_password.data(), current_password.size());
        memcpy(s + current_salt.size() + current_password.size(), current_salt.data(), current_salt.size());

        TGLC_sha256((const unsigned char *)s, 2 * current_salt.size() + current_password.size(), shab);
        q->out_string((const char *)shab, 32);
    } else {
        q->out_string("");
    }

    q->out_i32(CODE_account_password_input_settings);
    if (new_password.size()) {
        q->out_i32(1);

        char d[256];
        memset(d, 0, sizeof(d));
        memcpy(d, new_salt.data(), new_salt.size());

        int l = new_salt.size();
        tgl_secure_random((unsigned char*)d + l, 16);
        l += 16;
        memcpy(s, d, l);

        memcpy(s + l, new_password.data(), new_password.size());
        memcpy(s + l + new_password.size(), d, l);

        TGLC_sha256((const unsigned char *)s, 2 * l + new_password.size(), shab);

        q->out_string(d, l);
        q->out_string((const char *)shab, 32);
        q->out_string(hint.c_str(), hint.size());
    } else {
        q->out_i32(0);
    }

    q->execute(tgl_state::instance()->working_dc());
}

struct change_password_state {
    std::string current_password;
    std::string new_password;
    std::string current_salt;
    std::string new_salt;
    std::string hint;
    std::function<void(bool)> callback;
};

void tgl_on_new_pwd(const std::shared_ptr<change_password_state>& state, const void* answer)
{
    const char** pwds = (const char**)answer;
    state->new_password = std::string(pwds[0]);
    std::string new_password_confirm = std::string(pwds[1]);

    if (state->new_password != new_password_confirm) {
        TGL_ERROR("passwords do not match");
        tgl_state::instance()->callback()->get_values(tgl_value_type::new_password, "new password: ", 2, std::bind(tgl_on_new_pwd, state, std::placeholders::_1));
        return;
    }

    tgl_do_act_set_password(state->current_password,
            state->new_password,
            state->current_salt,
            state->new_salt,
            state->hint,
            state->callback);
}

void tgl_on_old_pwd(const std::shared_ptr<change_password_state>& state, const void* answer)
{
    const char** pwds = (const char**)answer;
    state->current_password = std::string(pwds[0]);
    tgl_on_new_pwd(state, pwds + 1);
}

class query_get_and_set_password: public query
{
public:
    query_get_and_set_password(const std::string& hint,
            const std::function<void(bool)>& callback)
        : query("get and set password", TYPE_TO_PARAM(account_password))
        , m_hint(hint)
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_account_password* DS_AP = static_cast<tl_ds_account_password*>(D);
        std::shared_ptr<change_password_state> state = std::make_shared<change_password_state>();

        if (DS_AP->current_salt && DS_AP->current_salt->data) {
            state->current_salt = std::string(DS_AP->current_salt->data, DS_AP->current_salt->len);
        }
        if (DS_AP->new_salt && DS_AP->new_salt->data) {
            state->new_salt = std::string(DS_AP->new_salt->data, DS_AP->new_salt->len);
        }

        if (!m_hint.empty()) {
            state->hint = m_hint;
        }

        state->callback = m_callback;

        if (DS_AP->magic == CODE_account_no_password) {
            tgl_state::instance()->callback()->get_values(tgl_value_type::new_password, "new password: ", 2, std::bind(tgl_on_new_pwd, state, std::placeholders::_1));
        } else {
            char s[512];
            memset(s, 0, sizeof(s));
            snprintf(s, sizeof(s) - 1, "old password (hint %.*s): ", DS_RSTR(DS_AP->hint));
            tgl_state::instance()->callback()->get_values(tgl_value_type::cur_and_new_password, s, 3, std::bind(tgl_on_old_pwd, state, std::placeholders::_1));
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::string m_hint;
    std::function<void(bool)> m_callback;
};

void tgl_do_set_password(const std::string& hint, const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_get_and_set_password>(hint, callback);
    q->out_i32(CODE_account_get_password);
    q->execute(tgl_state::instance()->working_dc());
}

/* }}} */

/* {{{ check password */
class query_check_password: public query
{
public:
    explicit query_check_password(const std::function<void(bool)>& callback)
        : query("check password", TYPE_TO_PARAM(auth_authorization))
        , m_callback(callback)
    { }

    virtual void on_answer(void*) override
    {
        tgl_state::instance()->set_password_locked(false);
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        if (error_code == 400) {
            TGL_ERROR("bad password");
            tgl_do_check_password(m_callback);
            return 0;
        }

        tgl_state::instance()->set_password_locked(false);
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);

        if (m_callback) {
            m_callback(false);
        }

        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

static void tgl_pwd_got(const std::string& current_salt, const std::function<void(bool)>& callback, const void* answer)
{
    char s[512];
    unsigned char shab[32];
    memset(s, 0, sizeof(s));
    memset(shab, 0, sizeof(shab));

    const char* pwd = static_cast<const char*>(answer);
    int pwd_len = pwd ? strlen(pwd) : 0;
    if (current_salt.size() > 128 || pwd_len > 128) {
        if (callback) {
            callback(false);
        }
        return;
    }

    auto q = std::make_shared<query_check_password>(callback);
    q->out_i32(CODE_auth_check_password);

    if (pwd && current_salt.size()) {
        memcpy(s, current_salt.data(), current_salt.size());
        memcpy(s + current_salt.size(), pwd, pwd_len);
        memcpy(s + current_salt.size() + pwd_len, current_salt.data(), current_salt.size());
        TGLC_sha256((const unsigned char *)s, 2 * current_salt.size() + pwd_len, shab);
        q->out_string((const char *)shab, 32);
    } else {
        q->out_string("");
    }

    q->execute(tgl_state::instance()->working_dc());
}

class query_get_and_check_password: public query
{
public:
    explicit query_get_and_check_password(const std::function<void(bool)>& callback)
        : query("get and check password", TYPE_TO_PARAM(account_password))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_account_password* DS_AP = static_cast<tl_ds_account_password*>(D);

        if (DS_AP->magic == CODE_account_no_password) {
            tgl_state::instance()->set_password_locked(false);
            return;
        }

        char s[512];
        memset(s, 0, sizeof(s));
        snprintf(s, sizeof(s) - 1, "type password (hint %.*s): ", DS_RSTR(DS_AP->hint));

        std::string current_salt;
        if (DS_AP->current_salt && DS_AP->current_salt->data) {
            current_salt = std::string(DS_AP->current_salt->data, DS_AP->current_salt->len);
        }

        tgl_state::instance()->callback()->get_values(tgl_value_type::cur_password, s, 1,
                std::bind(tgl_pwd_got, current_salt, m_callback, std::placeholders::_1));
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        tgl_state::instance()->set_password_locked(false);
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

static void tgl_do_check_password(const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_get_and_check_password>(callback);
    q->out_i32(CODE_account_get_password);
    q->execute(tgl_state::instance()->working_dc());
}

/* }}} */

/* {{{ send broadcast */
void tgl_do_send_broadcast(int num, tgl_input_peer_t peer_id[], const std::string& text, int text_len,
        const std::function<void(bool success, const std::vector<std::shared_ptr<tgl_message>>& ML)>& callback)
{
    if (num > 1000) {
        if (callback) {
            callback(false, std::vector<std::shared_ptr<tgl_message>>());
        }
        return;
    }

    std::shared_ptr<messages_send_extra> E = std::make_shared<messages_send_extra>();
    E->multi = true;
    E->count = num;

    for (int i = 0; i < num; i++) {
        assert(peer_id[i].peer_type == tgl_peer_type::user);

        int64_t message_id;
        tgl_secure_random(reinterpret_cast<unsigned char*>(&message_id), 8);
        E->message_ids.push_back(message_id);

        tgl_peer_id_t from_id = tgl_state::instance()->our_id();

        int64_t date = tgl_get_system_time();
        struct tl_ds_message_media TDSM;
        TDSM.magic = CODE_message_media_empty;

        auto msg = tglm_create_message(message_id, from_id, peer_id[i], nullptr, nullptr, &date, text, &TDSM, nullptr, 0, nullptr);
        msg->set_unread(true).set_outgoing(true).set_pending(true);
        tgl_state::instance()->callback()->new_messages({msg});
    }

    auto q = std::make_shared<query_send_msgs>(E, callback);
    q->out_i32(CODE_messages_send_broadcast);
    q->out_i32(CODE_vector);
    q->out_i32(num);
    for (int i = 0; i < num; i++) {
        assert(peer_id[i].peer_type == tgl_peer_type::user);

        q->out_i32(CODE_input_user);
        q->out_i32(peer_id[i].peer_id);
        q->out_i64(peer_id[i].access_hash);
    }

    q->out_i32(CODE_vector);
    q->out_i32(num);
    for (int i = 0; i < num; i++) {
        q->out_i64(E->message_ids[i]);
    }
    q->out_std_string(text);

    q->out_i32(CODE_message_media_empty);

    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ block user */
class query_block_or_unblock_user: public query
{
public:
    explicit query_block_or_unblock_user(const std::function<void(bool)>& callback)
        : query("block or unblock user", TYPE_TO_PARAM(bool))
        , m_callback(callback)
    { }

    virtual void on_answer(void*) override
    {
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

void tgl_do_block_user(const tgl_input_peer_t& id, const std::function<void(bool success)>& callback)
{
    if (id.peer_type != tgl_peer_type::user) {
        TGL_ERROR("id should be user id");
        if (callback) {
            callback(false);
        }
        return;
    }

    auto q = std::make_shared<query_block_or_unblock_user>(callback);
    q->out_i32(CODE_contacts_block);
    q->out_i32(CODE_input_user);
    q->out_i32(id.peer_id);
    q->out_i64(id.access_hash);
    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_unblock_user(const tgl_input_peer_t& id, const std::function<void(bool success)>& callback)
{
    if (id.peer_type != tgl_peer_type::user) {
        TGL_ERROR("id should be user id");
        if (callback) {
            callback(false);
        }
        return;
    }

    auto q = std::make_shared<query_block_or_unblock_user>(callback);
    q->out_i32(CODE_contacts_unblock);
    q->out_i32(CODE_input_user);
    q->out_i32(id.peer_id);
    q->out_i64(id.access_hash);
    q->execute(tgl_state::instance()->working_dc());
}

class query_blocked_users: public query
{
public:
    explicit query_blocked_users(const std::function<void(std::vector<int32_t>)>& callback)
        : query("get blocked users", TYPE_TO_PARAM(contacts_blocked))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        std::vector<int32_t> blocked_contacts;
        tl_ds_contacts_blocked* DS_T = static_cast<tl_ds_contacts_blocked*>(D);
        if (DS_T->blocked && DS_T->users) {
            for (int i=0; i<DS_LVAL(DS_T->blocked->cnt); ++i) {
                blocked_contacts.push_back(DS_LVAL(DS_T->blocked->data[i]->user_id));
                auto user = tglf_fetch_alloc_user(DS_T->users->data[i], false);
                user->set_blocked(true);
                tgl_state::instance()->callback()->new_user(user);
            }
        }
        if (m_callback) {
            m_callback(blocked_contacts);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback({});
        }
        return 0;
    }

private:
    std::function<void(std::vector<int32_t>)> m_callback;
};

void tgl_get_blocked_users(const std::function<void(std::vector<int32_t>)>& callback) {
    auto q = std::make_shared<query_blocked_users>(callback);
    q->out_i32(CODE_contacts_get_blocked);
    q->out_i32(0);
    q->out_i32(0);
    q->execute(tgl_state::instance()->working_dc());
}

/* }}} */

/* {{{ set notify settings */
class query_update_notify_settings: public query
{
public:
    explicit query_update_notify_settings(const std::function<void(bool)>& callback)
        : query("update notify settings", TYPE_TO_PARAM(bool))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

void tgl_do_update_notify_settings(
            const tgl_input_peer_t& peer_id,
            int32_t mute_until,
            const std::function<void(bool)>& callback)
{
    auto q = std::make_shared<query_update_notify_settings>(callback);
    q->out_i32(CODE_account_update_notify_settings);
    q->out_i32(CODE_input_notify_peer);
    q->out_input_peer(peer_id);
    q->out_i32(CODE_input_peer_notify_settings);
    q->out_i32(mute_until);
    q->out_std_string(std::string(""));
    q->out_i32(CODE_bool_true);
    q->out_i32(CODE_input_peer_notify_events_all);

    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ get notify settings */
class query_get_notify_settings: public query
{
public:
    explicit query_get_notify_settings(
            const std::function<void(bool, int32_t)>& callback)
        : query("get notify settings", TYPE_TO_PARAM(peer_notify_settings))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_peer_notify_settings* DS_CC = static_cast<tl_ds_peer_notify_settings*>(D);
        int mute_until = DS_LVAL(DS_CC->mute_until);

        if (m_callback) {
            m_callback(true, mute_until);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false, 0);
        }
        return 0;
    }

private:
    std::function<void(bool, int32_t)> m_callback;
};

void tgl_get_notify_settings(
            const tgl_input_peer_t &peer_id,
            const std::function<void(bool, int32_t mute_until)>& callback)
{
    auto q = std::make_shared<query_get_notify_settings>(callback);
    q->out_i32(CODE_account_get_notify_settings);
    q->out_i32(CODE_input_notify_peer);
    q->out_input_peer(peer_id);
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */

/* {{{ get terms of service */
class query_get_tos: public query
{
public:
    explicit query_get_tos(const std::function<void(bool, const std::string&)>& callback)
        : query("get tos", TYPE_TO_PARAM(help_terms_of_service))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_help_terms_of_service* DS_T = static_cast<tl_ds_help_terms_of_service*>(D);

        if (!DS_T->text || !DS_T->text->data) {
            if (m_callback) {
                m_callback(true, std::string());
            }
            return;
        }

        int l = DS_T->text->len;
        std::vector<char> buffer(l + 1);
        char* s = buffer.data();
        char* str = DS_T->text->data;
        int p = 0;
        int pp = 0;
        while (p < l) {
            if (*str == '\\' && p < l - 1) {
                str ++;
                p ++;
                switch (*str) {
                case 'n':
                    s[pp ++] = '\n';
                    break;
                case 't':
                    s[pp ++] = '\t';
                    break;
                case 'r':
                    s[pp ++] = '\r';
                    break;
                default:
                    s[pp ++] = *str;
                }
                str ++;
                p ++;
            } else {
                s[pp ++] = *str;
                str ++;
                p ++;
            }
        }
        s[pp] = 0;

        if (m_callback) {
            m_callback(true, std::string(s, pp));
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false, std::string());
        }
        return 0;
    }

private:
    std::function<void(bool, const std::string&)> m_callback;
};

void tgl_do_get_terms_of_service(const std::function<void(bool success, const std::string& tos)>& callback)
{
    auto q = std::make_shared<query_get_tos>(callback);
    q->out_i32(CODE_help_get_terms_of_service);
    q->out_string("");
    q->execute(tgl_state::instance()->working_dc());
}
/* }}} */
class query_register_device: public query
{
public:
    explicit query_register_device(const std::function<void(bool)>& callback)
        : query("regster device", TYPE_TO_PARAM(bool))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

void tgl_do_register_device(int token_type, const std::string& token,
        const std::string& device_model,
        const std::string& system_version,
        const std::string& app_version,
        bool app_sandbox,
        const std::string& lang_code,
        const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_register_device>(callback);
    q->out_i32(CODE_account_register_device);
    q->out_i32(token_type);
    q->out_std_string(token);
    q->out_std_string(device_model);
    q->out_std_string(system_version);
    q->out_std_string(app_version);
    q->out_i32(app_sandbox? CODE_bool_true : CODE_bool_false);
    q->out_std_string(lang_code);
    q->execute(tgl_state::instance()->working_dc());
}

void tgl_do_upgrade_group(const tgl_peer_id_t& id, const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_send_msgs>(callback);
    q->out_i32(CODE_messages_migrate_chat);
    q->out_i32(id.peer_id);
    q->execute(tgl_state::instance()->working_dc());
}


void tgl_do_set_dc_configured(const std::shared_ptr<tgl_dc>& dc, bool success)
{
    dc->set_configured(success);

    if (!success) {
        return;
    }

    TGL_DEBUG("DC " << dc->id << " is now configured");

    if (dc == tgl_state::instance()->working_dc() || dc->is_logged_in()) {
        dc->send_pending_queries();
    } else if (!dc->is_logged_in()) {
        if (dc->auth_transfer_in_process) {
            dc->send_pending_queries();
        } else {
            tgl_do_transfer_auth(dc, std::bind(tgl_transfer_auth_callback, dc, std::placeholders::_1));
        }
    }
}

void tgl_do_set_dc_logged_out(const std::shared_ptr<tgl_dc>& from_dc, bool success)
{
    if (from_dc->is_logging_out()) {
        tglq_query_delete(from_dc->logout_query_id());
        from_dc->set_logout_query_id(0);
    }

    if (!success) {
        return;
    }

    for (const auto& dc: tgl_state::instance()->dcs()) {
        if (!dc) {
            continue;
        }
        if (dc->session) {
            dc->session->clear();
            dc->session = nullptr;
        }
        if (dc->is_logging_out()) {
            tglq_query_delete(dc->logout_query_id());
            dc->set_logout_query_id(0);
        }
        dc->set_logged_in(false);
    }
    tgl_state::instance()->clear_all_locks();
}

class query_bind_temp_auth_key: public query
{
public:
    query_bind_temp_auth_key(const std::shared_ptr<tgl_dc>& dc, int64_t message_id)
        : query("bind temp auth key", TYPE_TO_PARAM(bool), message_id)
        , m_dc(dc)
    { }

    virtual void on_answer(void*) override
    {
        m_dc->set_bound();
        TGL_DEBUG("bind temp auth key successfully for DC " << m_dc->id);
        tgl_do_help_get_config_dc(m_dc);
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_WARNING("bind temp auth key error " << error_code << " " << error_string << " for DC " << m_dc->id);
        if (error_code == 400) {
            m_dc->restart_temp_authorization();
        }
        return 0;
    }

    virtual void on_timeout() override
    {
        TGL_WARNING("bind timed out for DC " << m_dc->id);
        m_dc->restart_temp_authorization();
    }

    virtual bool should_retry_on_timeout() override
    {
        return false;
    }

    virtual bool should_retry_after_recover_from_error() override
    {
        return false;
    }

private:
    std::shared_ptr<tgl_dc> m_dc;
};

void tgl_do_bind_temp_key(const std::shared_ptr<tgl_dc>& D, int64_t nonce, int32_t expires_at, void* data, int len, int64_t msg_id)
{
    auto q = std::make_shared<query_bind_temp_auth_key>(D, msg_id);
    q->out_i32(CODE_auth_bind_temp_auth_key);
    q->out_i64(D->auth_key_id);
    q->out_i64(nonce);
    q->out_i32(expires_at);
    q->out_string((char*)data, len);
    q->execute(D, query::execution_option::FORCE);
    assert(q->msg_id() == msg_id);
}

class query_update_status: public query
{
public:
    explicit query_update_status(const std::function<void(bool)>& callback)
        : query("update status", TYPE_TO_PARAM(bool))
        , m_callback(callback)
    { }

    virtual void on_answer(void*) override
    {
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

void tgl_do_update_status(bool online, const std::function<void(bool success)>& callback)
{
    auto q = std::make_shared<query_update_status>(callback);
    q->out_i32(CODE_account_update_status);
    q->out_i32(online ? CODE_bool_false : CODE_bool_true);
    q->execute(tgl_state::instance()->working_dc());
}

void tgl_started_cb(bool success)
{
    if (!success) {
        TGL_ERROR("login problem");
        tgl_state::instance()->callback()->on_failed_login();
        return;
    }

    if (!tgl_state::instance()->is_started()) {
        tgl_state::instance()->set_started(true);
        tgl_state::instance()->callback()->started();
    }
}

static void tgl_transfer_auth_callback(const std::shared_ptr<tgl_dc>& DC, bool success)
{
    assert(DC);
    DC->auth_transfer_in_process = false;
    if (!success) {
        TGL_ERROR("auth transfer problem to DC " << DC->id);
        return;
    }

    TGL_NOTICE("auth transferred from DC " << tgl_state::instance()->working_dc()->id << " to DC " << DC->id);
    DC->send_pending_queries();
}

void tgl_export_all_auth()
{
    for (const auto& dc: tgl_state::instance()->dcs()) {
        if (dc && !dc->is_logged_in()) {
            tgl_do_transfer_auth(dc, std::bind(tgl_transfer_auth_callback, dc, std::placeholders::_1));
        }
    }
}

void tgl_signed_in()
{
    tgl_state::instance()->callback()->logged_in();

    TGL_DEBUG("signed in, retrieving current server state");

    tgl_export_all_auth();
    tgl_started_cb(true);
    //tgl_do_get_difference(false, tgl_started_cb);
}

struct sign_up_extra {
    std::string phone;
    std::string hash;
    std::string first_name;
    std::string last_name;
};

void tgl_sign_in_phone(const void* phone);
void tgl_sign_in_code(const std::shared_ptr<sign_up_extra>& E, const void* code);
void tgl_sign_in_result(const std::shared_ptr<sign_up_extra>& E, bool success, const std::shared_ptr<tgl_user>& U)
{
    TGL_DEBUG("tgl_sign_in_result, success: " << std::boolalpha << success);
    if (!success) {
        TGL_ERROR("incorrect code");
        tgl_state::instance()->callback()->get_values(tgl_value_type::code, "code ('call' for phone call, 'resend' to resend the code):", 1, std::bind(tgl_sign_in_code, E, std::placeholders::_1));
        return;
    }
    tgl_signed_in();
}

void tgl_sign_in_code(const std::shared_ptr<sign_up_extra>& E, const void* code)
{
    if (!strcmp((const char *)code, "call")) {
        tgl_do_phone_call(E->phone, E->hash, nullptr);
        tgl_state::instance()->callback()->get_values(tgl_value_type::code, "code ('call' for phone call, 'resend' to resend the code):", 1, std::bind(tgl_sign_in_code, E, std::placeholders::_1));
        return;
    } else if (!strcmp((const char *)code, "resend")) {
        tgl_sign_in_phone(E->phone.c_str());
        return;
    }

    tgl_do_send_code_result(E->phone, E->hash, std::string(static_cast<const char*>(code)),
            std::bind(tgl_sign_in_result, E, std::placeholders::_1, std::placeholders::_2));
}

void tgl_sign_up_code(const std::shared_ptr<sign_up_extra>& E, const void* code);
void tgl_sign_up_result(const std::shared_ptr<sign_up_extra>& E, bool success, const std::shared_ptr<tgl_user>& U)
{
    TGL_UNUSED(U);
    if (!success) {
        TGL_ERROR("incorrect code");
        tgl_state::instance()->callback()->get_values(tgl_value_type::code, "code ('call' for phone call, 'resend' to resend the code):", 1, std::bind(tgl_sign_up_code, E, std::placeholders::_1));
        return;
    }
    tgl_signed_in();
}

void tgl_sign_up_code(const std::shared_ptr<sign_up_extra>& E, const void* code)
{
    if (!strcmp((const char*)code, "call")) {
        tgl_do_phone_call(E->phone, E->hash, nullptr);
        tgl_state::instance()->callback()->get_values(tgl_value_type::code, "code ('call' for phone call, 'resend' to resend the code):", 1, std::bind(tgl_sign_up_code, E, std::placeholders::_1));
        return;
    } else if (!strcmp((const char *)code, "resend")) {
        tgl_sign_in_phone(E->phone.c_str()); // there is no tgl_sign_up_phone(), so this is okay
        return;
    }

    tgl_do_send_code_result_auth(E->phone, E->hash, std::string(static_cast<const char*>(code)), E->first_name, E->last_name,
            std::bind(tgl_sign_up_result, E, std::placeholders::_1, std::placeholders::_2));
}

void tgl_register_cb(const std::shared_ptr<sign_up_extra>& E, const void* rinfo)
{
    const char** yn = (const char**)rinfo;
    if (yn[0]) {
        E->first_name = static_cast<const char*>(yn[1]);
        if (E->first_name.size() >= 1) {
            E->last_name = static_cast<const char*>(yn[2]);
            tgl_state::instance()->callback()->get_values(tgl_value_type::code, "code ('call' for phone call, 'resend' to resend the code):", 1, std::bind(tgl_sign_up_code, E, std::placeholders::_1));
        } else {
            tgl_state::instance()->callback()->get_values(tgl_value_type::register_info, "registration info:", 3, std::bind(tgl_register_cb, E, std::placeholders::_1));
        }
    } else {
        TGL_ERROR("stopping registration");
        tgl_state::instance()->login();
    }
}

void tgl_sign_in_phone_cb(const std::shared_ptr<sign_up_extra>& E, bool success, bool registered, const std::string& mhash)
{
    tgl_state::instance()->set_phone_number_input_locked(false);
    if (!success) {
        tgl_state::instance()->callback()->on_failed_login();
        E->phone = std::string();
        tgl_state::instance()->callback()->get_values(tgl_value_type::phone_number, "phone number:", 1, tgl_sign_in_phone);
        return;
    }

    E->hash = mhash;

    if (registered) {
        TGL_NOTICE("already registered, need code");
        tgl_state::instance()->callback()->get_values(tgl_value_type::code, "code ('call' for phone call, 'resend' to resend the code):", 1, std::bind(tgl_sign_in_code, E, std::placeholders::_1));
    } else {
        TGL_NOTICE("not registered");
        tgl_state::instance()->callback()->get_values(tgl_value_type::register_info, "registration info:", 3, std::bind(tgl_register_cb, E, std::placeholders::_1));
    }
}

void tgl_sign_in_phone(const void* phone)
{
    std::shared_ptr<sign_up_extra> E = std::make_shared<sign_up_extra>();
    E->phone = static_cast<const char*>(phone);

    tgl_state::instance()->set_phone_number_input_locked(true);

    tgl_do_send_code(E->phone, std::bind(tgl_sign_in_phone_cb, E, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
}

void tgl_bot_hash_cb(const void* code);

void tgl_sign_in_bot_cb(bool success, const std::shared_ptr<tgl_user>& U)
{
    if (!success) {
        TGL_ERROR("incorrect bot hash");
        tgl_state::instance()->callback()->get_values(tgl_value_type::bot_hash, "bot hash:", 1, tgl_bot_hash_cb);
        return;
    }
    tgl_signed_in();
}

void tgl_bot_hash_cb(const void* code)
{
    tgl_do_send_bot_auth((const char*)code, strlen((const char*)code), tgl_sign_in_bot_cb);
}

static void tgl_sign_in()
{
    assert(!tgl_state::instance()->working_dc()->is_logged_in());

    if (!tgl_state::instance()->is_phone_number_input_locked()) {
        TGL_DEBUG("asking for phone number");
        tgl_state::instance()->callback()->get_values(tgl_value_type::phone_number, "phone number:", 1, tgl_sign_in_phone);
    }
}

void tgl_state::login()
{
    std::shared_ptr<tgl_dc> dc = tgl_state::instance()->working_dc();
    if (!dc) {
        TGL_ERROR("no working dc set, can't log in");
        return;
    }

    if (!dc->is_authorized()) {
        dc->restart_authorization();
    }

    if (dc->is_logged_in()) {
        tgl_signed_in();
        return;
    }

    tgl_sign_in();
}

class query_set_phone: public query
{
public:
    explicit query_set_phone(const std::function<void(bool, const std::shared_ptr<tgl_user>&)>& callback)
        : query("set phone", TYPE_TO_PARAM(user))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        std::shared_ptr<tgl_user> user = tglf_fetch_alloc_user(static_cast<tl_ds_user*>(D));
        if (m_callback) {
            m_callback(true, user);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false, nullptr);
        }
        return 0;
    }

private:
    std::function<void(bool, const std::shared_ptr<tgl_user>&)> m_callback;
};

class query_send_change_code: public query
{
public:
    explicit query_send_change_code(const std::function<void(bool, const std::string&)>& callback)
        : query("send change phone code", TYPE_TO_PARAM(account_sent_change_phone_code))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_account_sent_change_phone_code* DS_ASCPC = static_cast<tl_ds_account_sent_change_phone_code*>(D);
        std::string phone_code_hash;
        if (DS_ASCPC->phone_code_hash && DS_ASCPC->phone_code_hash->data) {
            phone_code_hash = std::string(DS_ASCPC->phone_code_hash->data, DS_ASCPC->phone_code_hash->len);
        }
        if (m_callback) {
            m_callback(true, phone_code_hash);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false, std::string());
        }
        return 0;
    }

private:
    std::function<void(bool, const std::string&)> m_callback;
};

struct change_phone_state {
    std::string phone;
    std::string hash;
    std::string first_name;
    std::string last_name;
    std::function<void(bool success)> callback;
};

static void tgl_set_number_code(const std::shared_ptr<change_phone_state>& state, const void* code);

static void tgl_set_number_result(const std::shared_ptr<change_phone_state>& state, bool success, const std::shared_ptr<tgl_user>&)
{
    if (success) {
        if (state->callback) {
            state->callback(true);
        }
    } else {
        TGL_ERROR("incorrect code");
        tgl_state::instance()->callback()->get_values(tgl_value_type::code, "code:", 1, std::bind(tgl_set_number_code, state, std::placeholders::_1));
    }
}

static void tgl_set_number_code(const std::shared_ptr<change_phone_state>& state, const void* code)
{
    const char** code_strings = (const char**)code;

    auto q = std::make_shared<query_set_phone>(std::bind(tgl_set_number_result, state, std::placeholders::_1, std::placeholders::_2));
    q->out_i32(CODE_account_change_phone);
    q->out_string(state->phone.data(), state->phone.size());
    q->out_string(state->hash.data(), state->hash.size());
    q->out_string(code_strings[0], strlen(code_strings[0]));
    q->execute(tgl_state::instance()->working_dc());
}


static void tgl_set_phone_number_cb(const std::shared_ptr<change_phone_state>& state, bool success, const std::string& hash)
{
    if (!success) {
        TGL_ERROR("incorrect phone number");
        if (state->callback) {
            state->callback(false);
        }
        return;
    }

    state->hash = hash;
    tgl_state::instance()->callback()->get_values(tgl_value_type::code, "code:", 1, std::bind(tgl_set_number_code, state, std::placeholders::_1));
}

void tgl_do_set_phone_number(const std::string& phonenumber, const std::function<void(bool success)>& callback)
{
    std::shared_ptr<change_phone_state> state = std::make_shared<change_phone_state>();
    state->phone = phonenumber;
    state->callback = callback;

    auto q = std::make_shared<query_send_change_code>(std::bind(tgl_set_phone_number_cb, state, std::placeholders::_1, std::placeholders::_2));
    q->out_header();
    q->out_i32(CODE_account_send_change_phone_code);
    q->out_std_string(state->phone);
    q->execute(tgl_state::instance()->working_dc());
}

class query_privacy : public query
{
public:
    explicit query_privacy(const std::function<void(bool, const std::vector<std::pair<tgl_privacy_rule, const std::vector<int32_t>>>&)>& callback)
        : query("set phone", TYPE_TO_PARAM(account_privacy_rules))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        tl_ds_account_privacy_rules* rules = static_cast<tl_ds_account_privacy_rules*>(D);
        std::vector<std::pair<tgl_privacy_rule, const std::vector<int32_t>>> privacy_rules;
        if (rules->rules) {
            for (int32_t i=0; i<DS_LVAL(rules->rules->cnt); ++i) {
                uint32_t rule = rules->rules->data[i]->magic;
                std::vector<int32_t> users;
                tgl_privacy_rule tgl_rule;
                switch (rule) {
                case(CODE_privacy_value_allow_contacts): tgl_rule = tgl_privacy_rule::allow_contacts; break;
                case(CODE_privacy_value_allow_all): tgl_rule = tgl_privacy_rule::allow_all; break;
                case(CODE_privacy_value_allow_users): {
                    tgl_rule = tgl_privacy_rule::allow_users;
                    if (rules->rules->data[i]->users) {
                        for (int32_t i=0; i<DS_LVAL(rules->rules->data[i]->users->cnt); ++i) {
                            users.push_back(DS_LVAL(rules->rules->data[i]->users->data[i]));
                        }
                    }
                    break;
                }
                case(CODE_privacy_value_disallow_contacts): tgl_rule = tgl_privacy_rule::disallow_contacts; break;
                case(CODE_privacy_value_disallow_all): tgl_rule = tgl_privacy_rule::disallow_all; break;
                case(CODE_privacy_value_disallow_users): {
                    tgl_rule = tgl_privacy_rule::disallow_users;
                    if (rules->rules->data[i]->users) {
                        for (int32_t j=0; j<DS_LVAL(rules->rules->data[i]->users->cnt); ++j) {
                            users.push_back(DS_LVAL(rules->rules->data[i]->users->data[j]));
                        }
                    }
                    break;
                }
                default:    tgl_rule = tgl_privacy_rule::unknown;
                }

                privacy_rules.push_back(std::make_pair(tgl_rule, users));
            }
        }
        //std::shared_ptr<tgl_user> user = tglf_fetch_alloc_user(static_cast<tl_ds_user*>(D));
        if (m_callback) {
            m_callback(true, privacy_rules);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false, {});
        }
        return 0;
    }

private:
    std::function<void(bool, const std::vector<std::pair<tgl_privacy_rule, const std::vector<int32_t>>>&)> m_callback;
};


void tgl_do_get_privacy(std::function<void(bool, const std::vector<std::pair<tgl_privacy_rule, const std::vector<int32_t>>>&)> callback)
{
    auto q = std::make_shared<query_privacy>(callback);
    q->out_i32(CODE_account_get_privacy);
    q->out_i32(CODE_input_privacy_key_status_timestamp);
    q->execute(tgl_state::instance()->working_dc());
}

class query_send_inline_query_to_bot: public query
{
public:
    explicit query_send_inline_query_to_bot(const std::function<void(bool, const std::string&)>& callback)
        : query("send inline query to bot", TYPE_TO_PARAM(messages_bot_results))
        , m_callback(callback)
    { }

    virtual void on_answer(void* D) override
    {
        if (m_callback) {
            std::string response;
            tl_ds_messages_bot_results* bot_results = static_cast<tl_ds_messages_bot_results*>(D);
            if (bot_results->results && DS_LVAL(bot_results->results->cnt) == 1
                    && bot_results->results->data[0]->magic == CODE_bot_inline_result) {
                tl_ds_bot_inline_message* inline_message = bot_results->results->data[0]->send_message;
                if (inline_message && inline_message->magic == CODE_bot_inline_message_text) {
                    response = DS_STDSTR(inline_message->message);
                }
            }
            m_callback(true, response);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false, std::string());
        }
        return 0;
    }

private:
    std::function<void(bool, const std::string&)> m_callback;
};

void tgl_do_send_inline_query_to_bot(const tgl_input_peer_t& bot, const std::string& query,
        const std::function<void(bool success, const std::string& response)>& callback)
{
    auto q = std::make_shared<query_send_inline_query_to_bot>(callback);
    q->out_i32(CODE_messages_get_inline_bot_results);
    q->out_input_peer(bot);
    q->out_std_string(query);
    q->out_std_string(std::string());
    q->execute(tgl_state::instance()->working_dc());
}