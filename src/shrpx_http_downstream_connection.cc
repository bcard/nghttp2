/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "shrpx_http_downstream_connection.h"

#include "shrpx_client_handler.h"
#include "shrpx_upstream.h"
#include "shrpx_downstream.h"
#include "shrpx_config.h"
#include "shrpx_error.h"
#include "shrpx_http.h"
#include "shrpx_worker_config.h"
#include "shrpx_connect_blocker.h"
#include "shrpx_downstream_connection_pool.h"
#include "shrpx_worker.h"
#include "http2.h"
#include "util.h"

using namespace nghttp2;

namespace shrpx {

namespace {
void timeoutcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto dconn = static_cast<HttpDownstreamConnection *>(w->data);

  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, dconn) << "Time out";
  }

  auto downstream = dconn->get_downstream();
  auto upstream = downstream->get_upstream();
  auto handler = upstream->get_client_handler();

  // Do this so that dconn is not pooled
  downstream->set_response_connection_close(true);

  if (upstream->downstream_error(dconn, Downstream::EVENT_TIMEOUT) != 0) {
    delete handler;
  }
}
} // namespace

namespace {
void readcb(struct ev_loop *loop, ev_io *w, int revents) {
  auto dconn = static_cast<HttpDownstreamConnection *>(w->data);
  auto downstream = dconn->get_downstream();
  auto upstream = downstream->get_upstream();
  auto handler = upstream->get_client_handler();

  if (upstream->downstream_read(dconn) != 0) {
    delete handler;
  }
}
} // namespace

namespace {
void writecb(struct ev_loop *loop, ev_io *w, int revents) {
  auto dconn = static_cast<HttpDownstreamConnection *>(w->data);
  auto downstream = dconn->get_downstream();
  auto upstream = downstream->get_upstream();
  auto handler = upstream->get_client_handler();

  if (upstream->downstream_write(dconn) != 0) {
    delete handler;
  }
}
} // namespace

namespace {
void connectcb(struct ev_loop *loop, ev_io *w, int revents) {
  auto dconn = static_cast<HttpDownstreamConnection *>(w->data);
  auto downstream = dconn->get_downstream();
  auto upstream = downstream->get_upstream();
  auto handler = upstream->get_client_handler();
  if (dconn->on_connect() != 0) {
    if (upstream->on_downstream_abort_request(downstream, 503) != 0) {
      delete handler;
    }
    return;
  }
  writecb(loop, w, revents);
}
} // namespace

HttpDownstreamConnection::HttpDownstreamConnection(
    DownstreamConnectionPool *dconn_pool, struct ev_loop *loop)
    : DownstreamConnection(dconn_pool), rlimit_(loop, &rev_, 0, 0),
      ioctrl_(&rlimit_), response_htp_{0}, loop_(loop), fd_(-1) {
  // We do not know fd yet, so just set dummy fd 0
  ev_io_init(&wev_, connectcb, 0, EV_WRITE);
  ev_io_init(&rev_, readcb, 0, EV_READ);

  wev_.data = this;
  rev_.data = this;

  ev_timer_init(&wt_, timeoutcb, 0., get_config()->downstream_write_timeout);
  ev_timer_init(&rt_, timeoutcb, 0., get_config()->downstream_read_timeout);

  wt_.data = this;
  rt_.data = this;
}

HttpDownstreamConnection::~HttpDownstreamConnection() {
  ev_timer_stop(loop_, &rt_);
  ev_timer_stop(loop_, &wt_);
  ev_io_stop(loop_, &rev_);
  ev_io_stop(loop_, &wev_);

  if (fd_ != -1) {
    shutdown(fd_, SHUT_WR);
    close(fd_);
  }
  // Downstream and DownstreamConnection may be deleted
  // asynchronously.
  if (downstream_) {
    downstream_->release_downstream_connection();
  }
}

int HttpDownstreamConnection::attach_downstream(Downstream *downstream) {
  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Attaching to DOWNSTREAM:" << downstream;
  }

  if (fd_ == -1) {
    auto connect_blocker = client_handler_->get_http1_connect_blocker();

    if (connect_blocker->blocked()) {
      if (LOG_ENABLED(INFO)) {
        DCLOG(INFO, this)
            << "Downstream connection was blocked by connect_blocker";
      }
      return -1;
    }

    auto worker_stat = client_handler_->get_worker_stat();
    auto end = worker_stat->next_downstream;
    for (;;) {
      auto i = worker_stat->next_downstream;
      ++worker_stat->next_downstream;
      worker_stat->next_downstream %= get_config()->downstream_addrs.size();

      fd_ = util::create_nonblock_socket(
          get_config()->downstream_addrs[i].addr.storage.ss_family);

      if (fd_ == -1) {
        auto error = errno;
        DCLOG(WARN, this) << "socket() failed; errno=" << error;

        connect_blocker->on_failure();

        return SHRPX_ERR_NETWORK;
      }

      int rv;
      rv = connect(fd_, const_cast<sockaddr *>(
                            &get_config()->downstream_addrs[i].addr.sa),
                   get_config()->downstream_addrs[i].addrlen);
      if (rv != 0 && errno != EINPROGRESS) {
        auto error = errno;
        DCLOG(WARN, this) << "connect() failed; errno=" << error;

        connect_blocker->on_failure();
        close(fd_);
        fd_ = -1;

        if (end == worker_stat->next_downstream) {
          return SHRPX_ERR_NETWORK;
        }

        // Try again with the next downstream server
        continue;
      }

      if (LOG_ENABLED(INFO)) {
        DCLOG(INFO, this) << "Connecting to downstream server";
      }

      ev_io_set(&wev_, fd_, EV_WRITE);
      ev_io_set(&rev_, fd_, EV_READ);

      ev_io_start(loop_, &wev_);

      break;
    }
  }

  downstream_ = downstream;

  http_parser_init(&response_htp_, HTTP_RESPONSE);
  response_htp_.data = downstream_;

  ev_set_cb(&rev_, readcb);

  rt_.repeat = get_config()->downstream_read_timeout;
  ev_timer_again(loop_, &rt_);
  // TODO we should have timeout for connection establishment
  ev_timer_again(loop_, &wt_);

  return 0;
}

int HttpDownstreamConnection::push_request_headers() {
  downstream_->assemble_request_cookie();

  // Assume that method and request path do not contain \r\n.
  std::string hdrs = downstream_->get_request_method();
  hdrs += " ";
  if (downstream_->get_request_method() == "CONNECT") {
    if (!downstream_->get_request_http2_authority().empty()) {
      hdrs += downstream_->get_request_http2_authority();
    } else {
      hdrs += downstream_->get_request_path();
    }
  } else if (get_config()->http2_proxy &&
             !downstream_->get_request_http2_scheme().empty() &&
             !downstream_->get_request_http2_authority().empty() &&
             (downstream_->get_request_path().c_str()[0] == '/' ||
              downstream_->get_request_path() == "*")) {
    // Construct absolute-form request target because we are going to
    // send a request to a HTTP/1 proxy.
    hdrs += downstream_->get_request_http2_scheme();
    hdrs += "://";
    hdrs += downstream_->get_request_http2_authority();

    // Server-wide OPTIONS takes following form in proxy request:
    //
    // OPTIONS http://example.org HTTP/1.1
    //
    // Notice that no slash after authority. See
    // http://tools.ietf.org/html/rfc7230#section-5.3.4
    if (downstream_->get_request_path() != "*") {
      hdrs += downstream_->get_request_path();
    }
  } else {
    // No proxy case. get_request_path() may be absolute-form but we
    // don't care.
    hdrs += downstream_->get_request_path();
  }
  hdrs += " HTTP/1.1\r\n";
  if (!downstream_->get_request_header(http2::HD_HOST) &&
      !downstream_->get_request_http2_authority().empty()) {
    hdrs += "Host: ";
    hdrs += downstream_->get_request_http2_authority();
    hdrs += "\r\n";
  }
  http2::build_http1_headers_from_headers(hdrs,
                                          downstream_->get_request_headers());

  if (!downstream_->get_assembled_request_cookie().empty()) {
    hdrs += "Cookie: ";
    hdrs += downstream_->get_assembled_request_cookie();
    hdrs += "\r\n";
  }

  if (downstream_->get_request_method() != "CONNECT" &&
      downstream_->get_request_http2_expect_body() &&
      !downstream_->get_request_header(http2::HD_CONTENT_LENGTH)) {

    downstream_->set_chunked_request(true);
    hdrs += "Transfer-Encoding: chunked\r\n";
  }

  if (downstream_->get_request_connection_close()) {
    hdrs += "Connection: close\r\n";
  }
  auto xff = downstream_->get_request_header(http2::HD_X_FORWARDED_FOR);
  if (get_config()->add_x_forwarded_for) {
    hdrs += "X-Forwarded-For: ";
    if (xff && !get_config()->strip_incoming_x_forwarded_for) {
      hdrs += (*xff).value;
      hdrs += ", ";
    }
    hdrs += client_handler_->get_ipaddr();
    hdrs += "\r\n";
  } else if (xff && !get_config()->strip_incoming_x_forwarded_for) {
    hdrs += "X-Forwarded-For: ";
    hdrs += (*xff).value;
    hdrs += "\r\n";
  }
  if (!get_config()->http2_proxy && !get_config()->client_proxy &&
      downstream_->get_request_method() != "CONNECT") {
    hdrs += "X-Forwarded-Proto: ";
    if (!downstream_->get_request_http2_scheme().empty()) {
      hdrs += downstream_->get_request_http2_scheme();
      hdrs += "\r\n";
    } else if (client_handler_->get_ssl()) {
      hdrs += "https\r\n";
    } else {
      hdrs += "http\r\n";
    }
  }
  auto expect = downstream_->get_request_header(http2::HD_EXPECT);
  if (expect && !util::strifind((*expect).value.c_str(), "100-continue")) {
    hdrs += "Expect: ";
    hdrs += (*expect).value;
    hdrs += "\r\n";
  }
  auto via = downstream_->get_request_header(http2::HD_VIA);
  if (get_config()->no_via) {
    if (via) {
      hdrs += "Via: ";
      hdrs += (*via).value;
      hdrs += "\r\n";
    }
  } else {
    hdrs += "Via: ";
    if (via) {
      hdrs += (*via).value;
      hdrs += ", ";
    }
    hdrs += http::create_via_header_value(downstream_->get_request_major(),
                                          downstream_->get_request_minor());
    hdrs += "\r\n";
  }

  hdrs += "\r\n";
  if (LOG_ENABLED(INFO)) {
    const char *hdrp;
    std::string nhdrs;
    if (worker_config->errorlog_tty) {
      nhdrs = http::colorizeHeaders(hdrs.c_str());
      hdrp = nhdrs.c_str();
    } else {
      hdrp = hdrs.c_str();
    }
    DCLOG(INFO, this) << "HTTP request headers. stream_id="
                      << downstream_->get_stream_id() << "\n" << hdrp;
  }
  auto output = downstream_->get_request_buf();
  output->append(hdrs.c_str(), hdrs.size());

  signal_write();

  return 0;
}

int HttpDownstreamConnection::push_upload_data_chunk(const uint8_t *data,
                                                     size_t datalen) {
  auto chunked = downstream_->get_chunked_request();
  auto output = downstream_->get_request_buf();

  if (chunked) {
    auto chunk_size_hex = util::utox(datalen);
    output->append(chunk_size_hex.c_str(), chunk_size_hex.size());
    output->append("\r\n");
  }

  output->append(data, datalen);

  if (chunked) {
    output->append("\r\n");
  }

  signal_write();

  return 0;
}

int HttpDownstreamConnection::end_upload_data() {
  if (!downstream_->get_chunked_request()) {
    return 0;
  }

  auto output = downstream_->get_request_buf();
  output->append("0\r\n\r\n");

  signal_write();

  return 0;
}

namespace {
void idle_readcb(struct ev_loop *loop, ev_io *w, int revents) {
  auto dconn = static_cast<HttpDownstreamConnection *>(w->data);
  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, dconn) << "Idle connection EOF";
  }
  auto dconn_pool = dconn->get_dconn_pool();
  dconn_pool->remove_downstream_connection(dconn);
  // dconn was deleted
}
} // namespace

namespace {
void idle_timeoutcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto dconn = static_cast<HttpDownstreamConnection *>(w->data);
  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, dconn) << "Idle connection timeout";
  }
  auto dconn_pool = dconn->get_dconn_pool();
  dconn_pool->remove_downstream_connection(dconn);
  // dconn was deleted
}
} // namespace

void HttpDownstreamConnection::detach_downstream(Downstream *downstream) {
  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Detaching from DOWNSTREAM:" << downstream;
  }
  downstream_ = nullptr;
  ioctrl_.force_resume_read();

  ev_io_start(loop_, &rev_);
  ev_io_stop(loop_, &wev_);

  ev_set_cb(&rev_, idle_readcb);

  ev_timer_stop(loop_, &wt_);

  rt_.repeat = get_config()->downstream_idle_read_timeout;
  ev_set_cb(&rt_, idle_timeoutcb);
  ev_timer_again(loop_, &rt_);
}

void HttpDownstreamConnection::pause_read(IOCtrlReason reason) {
  ioctrl_.pause_read(reason);
}

int HttpDownstreamConnection::resume_read(IOCtrlReason reason,
                                          size_t consumed) {
  if (!downstream_->response_buf_full()) {
    ioctrl_.resume_read(reason);
  }

  return 0;
}

void HttpDownstreamConnection::force_resume_read() {
  ioctrl_.force_resume_read();
}

namespace {
int htp_msg_begincb(http_parser *htp) {
  auto downstream = static_cast<Downstream *>(htp->data);

  if (downstream->get_response_state() != Downstream::INITIAL) {
    return -1;
  }

  return 0;
}
} // namespace

namespace {
int htp_hdrs_completecb(http_parser *htp) {
  auto downstream = static_cast<Downstream *>(htp->data);
  auto upstream = downstream->get_upstream();
  int rv;

  downstream->set_response_http_status(htp->status_code);
  downstream->set_response_major(htp->http_major);
  downstream->set_response_minor(htp->http_minor);

  if (downstream->index_response_headers() != 0) {
    downstream->set_response_state(Downstream::MSG_BAD_HEADER);
    return -1;
  }

  if (downstream->get_non_final_response()) {
    // For non-final response code, we just call
    // on_downstream_header_complete() without changing response
    // state.
    rv = upstream->on_downstream_header_complete(downstream);

    if (rv != 0) {
      return -1;
    }

    return 0;
  }

  downstream->set_response_connection_close(!http_should_keep_alive(htp));
  downstream->set_response_state(Downstream::HEADER_COMPLETE);
  downstream->inspect_http1_response();
  downstream->check_upgrade_fulfilled();
  if (downstream->get_upgraded()) {
    downstream->set_response_connection_close(true);
  }
  if (upstream->on_downstream_header_complete(downstream) != 0) {
    return -1;
  }

  if (downstream->get_upgraded()) {
    // Upgrade complete, read until EOF in both ends
    if (upstream->resume_read(SHRPX_MSG_BLOCK, downstream, 0) != 0) {
      return -1;
    }
    downstream->set_request_state(Downstream::HEADER_COMPLETE);
    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "HTTP upgrade success. stream_id="
                << downstream->get_stream_id();
    }
  }

  unsigned int status = downstream->get_response_http_status();
  // Ignore the response body. HEAD response may contain
  // Content-Length or Transfer-Encoding: chunked.  Some server send
  // 304 status code with nonzero Content-Length, but without response
  // body. See
  // http://tools.ietf.org/html/draft-ietf-httpbis-p1-messaging-20#section-3.3

  // TODO It seems that the cases other than HEAD are handled by
  // http-parser.  Need test.
  return downstream->get_request_method() == "HEAD" ||
                 (100 <= status && status <= 199) || status == 204 ||
                 status == 304
             ? 1
             : 0;
}
} // namespace

namespace {
int htp_hdr_keycb(http_parser *htp, const char *data, size_t len) {
  auto downstream = static_cast<Downstream *>(htp->data);
  if (downstream->get_response_state() != Downstream::INITIAL) {
    // ignore trailers
    return 0;
  }
  if (downstream->get_response_header_key_prev()) {
    downstream->append_last_response_header_key(data, len);
  } else {
    downstream->add_response_header(std::string(data, len), "");
  }
  if (downstream->get_response_headers_sum() > Downstream::MAX_HEADERS_SUM) {
    if (LOG_ENABLED(INFO)) {
      DLOG(INFO, downstream) << "Too large header block size="
                             << downstream->get_response_headers_sum();
    }
    return -1;
  }
  return 0;
}
} // namespace

namespace {
int htp_hdr_valcb(http_parser *htp, const char *data, size_t len) {
  auto downstream = static_cast<Downstream *>(htp->data);
  if (downstream->get_response_state() != Downstream::INITIAL) {
    // ignore trailers
    return 0;
  }
  if (downstream->get_response_header_key_prev()) {
    downstream->set_last_response_header_value(std::string(data, len));
  } else {
    downstream->append_last_response_header_value(data, len);
  }
  if (downstream->get_response_headers_sum() > Downstream::MAX_HEADERS_SUM) {
    if (LOG_ENABLED(INFO)) {
      DLOG(INFO, downstream) << "Too large header block size="
                             << downstream->get_response_headers_sum();
    }
    return -1;
  }
  return 0;
}
} // namespace

namespace {
int htp_bodycb(http_parser *htp, const char *data, size_t len) {
  auto downstream = static_cast<Downstream *>(htp->data);

  downstream->add_response_bodylen(len);

  return downstream->get_upstream()->on_downstream_body(
      downstream, reinterpret_cast<const uint8_t *>(data), len, true);
}
} // namespace

namespace {
int htp_msg_completecb(http_parser *htp) {
  auto downstream = static_cast<Downstream *>(htp->data);

  if (downstream->get_non_final_response()) {
    downstream->reset_response();

    return 0;
  }

  downstream->set_response_state(Downstream::MSG_COMPLETE);
  // Block reading another response message from (broken?)
  // server. This callback is not called if the connection is
  // tunneled.
  downstream->pause_read(SHRPX_MSG_BLOCK);
  return downstream->get_upstream()->on_downstream_body_complete(downstream);
}
} // namespace

namespace {
http_parser_settings htp_hooks = {
    htp_msg_begincb,     // http_cb on_message_begin;
    nullptr,             // http_data_cb on_url;
    nullptr,             // http_data_cb on_status;
    htp_hdr_keycb,       // http_data_cb on_header_field;
    htp_hdr_valcb,       // http_data_cb on_header_value;
    htp_hdrs_completecb, // http_cb      on_headers_complete;
    htp_bodycb,          // http_data_cb on_body;
    htp_msg_completecb   // http_cb      on_message_complete;
};
} // namespace

int HttpDownstreamConnection::on_read() {
  ev_timer_again(loop_, &rt_);
  uint8_t buf[8192];
  int rv;

  if (downstream_->get_upgraded()) {
    // For upgraded connection, just pass data to the upstream.
    for (;;) {
      ssize_t nread;
      while ((nread = read(fd_, buf, sizeof(buf))) == -1 && errno == EINTR)
        ;
      if (nread == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return 0;
        }
        return DownstreamConnection::ERR_NET;
      }

      if (nread == 0) {
        return DownstreamConnection::ERR_EOF;
      }

      rv = downstream_->get_upstream()->on_downstream_body(downstream_, buf,
                                                           nread, true);
      if (rv != 0) {
        return rv;
      }

      if (downstream_->response_buf_full()) {
        downstream_->pause_read(SHRPX_NO_BUFFER);
        return 0;
      }
    }
  }

  for (;;) {
    ssize_t nread;
    while ((nread = read(fd_, buf, sizeof(buf))) == -1 && errno == EINTR)
      ;
    if (nread == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
      }
      return DownstreamConnection::ERR_NET;
    }

    if (nread == 0) {
      return DownstreamConnection::ERR_EOF;
    }

    auto nproc = http_parser_execute(&response_htp_, &htp_hooks,
                                     reinterpret_cast<char *>(buf), nread);

    if (nproc != static_cast<size_t>(nread)) {
      if (LOG_ENABLED(INFO)) {
        DCLOG(INFO, this) << "nproc != nread";
      }
      return -1;
    }

    auto htperr = HTTP_PARSER_ERRNO(&response_htp_);

    if (htperr != HPE_OK) {
      if (LOG_ENABLED(INFO)) {
        DCLOG(INFO, this) << "HTTP parser failure: "
                          << "(" << http_errno_name(htperr) << ") "
                          << http_errno_description(htperr);
      }

      return -1;
    }

    if (downstream_->response_buf_full()) {
      downstream_->pause_read(SHRPX_NO_BUFFER);
      return 0;
    }
  }
}

#define DEFAULT_WR_IOVCNT 16

#if defined(IOV_MAX) && IOV_MAX < DEFAULT_WR_IOVCNT
#define MAX_WR_IOVCNT IOV_MAX
#else // !defined(IOV_MAX) || IOV_MAX >= DEFAULT_WR_IOVCNT
#define MAX_WR_IOVCNT DEFAULT_WR_IOVCNT
#endif // !defined(IOV_MAX) || IOV_MAX >= DEFAULT_WR_IOVCNT

int HttpDownstreamConnection::on_write() {
  ev_timer_again(loop_, &rt_);

  auto upstream = downstream_->get_upstream();
  auto input = downstream_->get_request_buf();

  struct iovec iov[MAX_WR_IOVCNT];

  while (input->rleft() > 0) {
    auto iovcnt = input->riovec(iov, util::array_size(iov));

    ssize_t nwrite;
    while ((nwrite = writev(fd_, iov, iovcnt)) == -1 && errno == EINTR)
      ;
    if (nwrite == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        ev_io_start(loop_, &wev_);
        ev_timer_again(loop_, &wt_);
        goto end;
      }
      return DownstreamConnection::ERR_NET;
    }
    input->drain(nwrite);
  }

  if (input->rleft() == 0) {
    ev_io_stop(loop_, &wev_);
    ev_timer_stop(loop_, &wt_);
  } else {
    ev_io_start(loop_, &wev_);
    ev_timer_again(loop_, &wt_);
  }

end:
  if (input->rleft() == 0) {
    upstream->resume_read(SHRPX_NO_BUFFER, downstream_,
                          downstream_->get_request_datalen());
  }

  return 0;
}

int HttpDownstreamConnection::on_connect() {
  auto connect_blocker = client_handler_->get_http1_connect_blocker();

  if (!util::check_socket_connected(fd_)) {
    ev_io_stop(loop_, &wev_);

    if (LOG_ENABLED(INFO)) {
      DLOG(INFO, this) << "downstream connect failed";
    }
    connect_blocker->on_failure();
    return -1;
  }

  connect_blocker->on_success();

  ev_io_start(loop_, &rev_);
  ev_set_cb(&wev_, writecb);

  return 0;
}

void HttpDownstreamConnection::on_upstream_change(Upstream *upstream) {}

void HttpDownstreamConnection::signal_write() { ev_io_start(loop_, &wev_); }

} // namespace shrpx
