/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2016 Tatsuhiro Tsujikawa
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
#include "shrpx_health_monitor_downstream_connection.h"

#include "shrpx_client_handler.h"
#include "shrpx_upstream.h"
#include "shrpx_downstream.h"
#include "shrpx_log.h"

namespace shrpx {

HealthMonitorDownstreamConnection::HealthMonitorDownstreamConnection() {}

HealthMonitorDownstreamConnection::~HealthMonitorDownstreamConnection() {}

int HealthMonitorDownstreamConnection::attach_downstream(
  Downstream *downstream) {
  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Attaching to DOWNSTREAM:" << downstream;
  }

  downstream_ = downstream;

  return 0;
}

void HealthMonitorDownstreamConnection::detach_downstream(
  Downstream *downstream) {
  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Detaching from DOWNSTREAM:" << downstream;
  }
  downstream_ = nullptr;
}

int HealthMonitorDownstreamConnection::push_request_headers() {
  downstream_->set_request_header_sent(true);
  auto src = downstream_->get_blocked_request_buf();
  auto dest = downstream_->get_request_buf();
  src->remove(*dest);

  return 0;
}

int HealthMonitorDownstreamConnection::push_upload_data_chunk(
  const uint8_t *data, size_t datalen) {
  return 0;
}

int HealthMonitorDownstreamConnection::end_upload_data() {
  auto upstream = downstream_->get_upstream();
  auto &resp = downstream_->response();

  resp.http_status = 200;

  resp.fs.add_header_token("content-length"sv, "0"sv, false,
                           http2::HD_CONTENT_LENGTH);

  if (upstream->send_reply(downstream_, nullptr, 0) != 0) {
    return -1;
  }

  return 0;
}

void HealthMonitorDownstreamConnection::pause_read(IOCtrlReason reason) {}

int HealthMonitorDownstreamConnection::resume_read(IOCtrlReason reason,
                                                   size_t consumed) {
  return 0;
}

void HealthMonitorDownstreamConnection::force_resume_read() {}

int HealthMonitorDownstreamConnection::on_read() { return 0; }

int HealthMonitorDownstreamConnection::on_write() { return 0; }

void HealthMonitorDownstreamConnection::on_upstream_change(Upstream *upstream) {
}

bool HealthMonitorDownstreamConnection::poolable() const { return false; }

const std::shared_ptr<DownstreamAddrGroup> &
HealthMonitorDownstreamConnection::get_downstream_addr_group() const {
  static std::shared_ptr<DownstreamAddrGroup> s;
  return s;
}

DownstreamAddr *HealthMonitorDownstreamConnection::get_addr() const {
  return nullptr;
}

} // namespace shrpx
