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

    Copyright Topology LP 2016
*/

#include "tgl-dc.h"

#include <algorithm>

#include "mtproto-client.h"
#include "queries.h"
#include "tgl-net.h"
#include "tgl-timer.h"

static const float SESSION_CLEANUP_TIMEOUT = 5.0;

void tgl_session::clear()
{
    dc.reset();
    session_id = 0;
    last_msg_id = 0;
    seq_no = 0;
    received_messages = 0;
    c->close();
    c = nullptr;
    ack_tree.clear();
    ev->cancel();
    ev = nullptr;
}

tgl_dc::tgl_dc()
    : id(0)
    , flags(0)
    , rsa_key_idx(0)
    , state(st_init)
    , auth_key_id(0)
    , temp_auth_key_id(0)
    , temp_auth_key_bind_query_id(0)
    , server_salt(0)
    , server_time_delta(0)
    , server_time_udelta(0)
    , auth_transfer_in_process(false)
    , session_cleanup_timer(tgl_state::instance()->timer_factory()->create_timer(std::bind(&tgl_dc::cleanup_timer_expired, this)))
{
    memset(auth_key, 0, 256);
    memset(temp_auth_key, 0, 256);
    memset(nonce, 0, 256);
    memset(new_nonce, 0, 256);
    memset(server_nonce, 0, 256);
}

void tgl_dc::reset()
{
    TGL_DEBUG("resetting DC " << id);
    if (session) {
        session->c->close();
        session->ev->cancel();
        session->c = nullptr;
        session->ev = nullptr;
        session = nullptr;
    }
    if (temp_auth_key_bind_query_id) {
        tglq_query_delete(temp_auth_key_bind_query_id);
        temp_auth_key_bind_query_id = 0;
    }
    flags = 0;
    rsa_key_idx = 0;
    state = st_init;
    memset(auth_key, 0, 256);
    memset(temp_auth_key, 0, 256);
    memset(nonce, 0, 256);
    memset(new_nonce, 0, 256);
    memset(server_nonce, 0, 256);
    auth_key_id = 0;
    temp_auth_key_id = 0;
    server_salt = 0;
    if (!pending_queries.empty()) {
        send_pending_queries();
    }
}

void tgl_dc::send_pending_queries() {
    TGL_NOTICE("sending pending queries for DC " << id);
    std::list<std::shared_ptr<query>> queries = pending_queries; // make a copy since queries can get re-enqueued
    for (std::shared_ptr<query> q : queries) {
        if (q->execute_after_pending()) {
            pending_queries.remove(q);
        } else {
            TGL_DEBUG("sending pending query failed for DC " << id);
        }
    }
}

void tgl_dc::add_query(std::shared_ptr<query> q) {
    active_queries.push_back(q);
    session_cleanup_timer->cancel();
}

void tgl_dc::remove_query(std::shared_ptr<query> q) {
    active_queries.remove(q);

    if (active_queries.empty() && pending_queries.empty() && tgl_state::instance()->working_dc().get() != this) {
        session_cleanup_timer->start(SESSION_CLEANUP_TIMEOUT);
    }
}

void tgl_dc::add_pending_query(std::shared_ptr<query> q) {
    if (std::find(pending_queries.cbegin(), pending_queries.cend(), q) == pending_queries.cend()) {
        pending_queries.push_back(q);
    }
}

void tgl_dc::remove_pending_query(std::shared_ptr<query> q) {
    pending_queries.remove(q);
}

void tgl_dc::cleanup_timer_expired() {
    if (active_queries.empty() && pending_queries.empty()) {
        TGL_DEBUG("cleanup timer expired for DC " << id << ", deleting session");
        if (session) {
            session->c->close();
            session->ev->cancel();
            session->c = nullptr;
            session->ev = nullptr;
            session = nullptr;
        }
    }
}
