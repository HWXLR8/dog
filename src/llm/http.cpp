#include "llm/http.hpp"
#include <curl/curl.h>
#include "util.hpp"

namespace dog {

namespace {

size_t write_cb_chunk(char* ptr, size_t size, size_t nmemb, void* ud) {
  auto* on = static_cast<std::function<void(const char*, size_t)>*>(ud);
  size_t n = size * nmemb;
  (*on)(ptr, n);
  return n;
}

size_t write_cb_vec(char* ptr, size_t size, size_t nmemb, void* ud) {
  auto* out = static_cast<std::string*>(ud);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

size_t header_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
  auto* out = static_cast<std::vector<std::string>*>(ud);
  size_t n = size * nmemb;
  std::string line(ptr, n);
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if (!line.empty() && line.back() == '\n') line.pop_back();
  if (!line.empty()) out->push_back(line);
  return n;
}

struct TransferResult {
  CURLcode code = CURLE_OK;
  bool interrupted = false;
  long status = 0;
};

// Drive a transfer through the non-blocking multi interface instead of
// curl_easy_perform. The blocking call ignores an in-flight interrupt: libcurl
// sits in its own select() waiting for socket data and only calls the progress
// callback when bytes actually arrive, so while the model is "thinking" (headers
// sent, no tokens yet) the interrupt flag is never observed until a timeout. The
// multi loop below re-checks util::interrupted() every iteration (capping the
// wait at 200 ms) so an interrupt is honored at any moment, stalled or not.
TransferResult run_transfer(CURL* c) {
  TransferResult tr;
  CURLM* m = curl_multi_init();
  int still = 1;
  curl_multi_add_handle(m, c);
  do {
    curl_multi_perform(m, &still);
    if (util::interrupted()) {
      tr.interrupted = true;
      break;
    }
    long wait_ms = 0;
    curl_multi_timeout(m, &wait_ms);
    if (wait_ms < 1) wait_ms = 1;    // "act now" -> one tick, then re-check
    if (wait_ms > 200) wait_ms = 200;  // bound so we re-check the interrupt promptly
    curl_multi_wait(m, nullptr, 0, wait_ms, nullptr);
  } while (still);
  CURLMsg* msg = curl_multi_info_read(m, &still);
  while (msg) {
    if (msg->msg == CURLMSG_DONE) {
      tr.code = msg->data.result;
      if (tr.code == CURLE_OK)
        curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &tr.status);
    }
    msg = curl_multi_info_read(m, &still);
  }
  curl_multi_remove_handle(m, c);
  curl_multi_cleanup(m);
  return tr;
}

void cleanup(CURL* c, curl_slist* hdr) {
  if (hdr) curl_slist_free_all(hdr);
  if (c) curl_easy_cleanup(c);
}

// Run a configured transfer; on success return its HTTP status, else 0 with *err
// set ("interrupted" if the user cancelled).
int finish(CURL* c, curl_slist* hdr, const TransferResult& tr, std::string* err) {
  if (tr.interrupted) {
    if (err) *err = "interrupted";
    cleanup(c, hdr);
    return 0;
  }
  if (tr.code != CURLE_OK) {
    if (err) *err = std::string(curl_easy_strerror(tr.code));
    cleanup(c, hdr);
    return 0;
  }
  long status = tr.status;
  cleanup(c, hdr);
  return (int)status;
}

}  // namespace

int http_post(const std::string& url,
              const std::map<std::string, std::string>& headers,
              const std::string& body,
              std::function<void(const char*, size_t)> on_chunk,
              std::string* err) {
  CURL* c = curl_easy_init();
  if (!c) {
    if (err) *err = "curl init failed";
    return 0;
  }
  curl_slist* hdr = nullptr;
  for (auto& [k, v] : headers) hdr = curl_slist_append(hdr, (k + ": " + v).c_str());

  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_POST, 1L);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
  if (hdr) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb_chunk);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &on_chunk);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);

  return finish(c, hdr, run_transfer(c), err);
}

int http_get(const std::string& url,
             const std::map<std::string, std::string>& headers,
             std::string* body,
             std::string* err) {
  CURL* c = curl_easy_init();
  if (!c) {
    if (err) *err = "curl init failed";
    return 0;
  }
  curl_slist* hdr = nullptr;
  for (auto& [k, v] : headers) hdr = curl_slist_append(hdr, (k + ": " + v).c_str());
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
  if (hdr) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb_vec);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);

  return finish(c, hdr, run_transfer(c), err);
}

int http_post_ex(const std::string& url,
                 const std::map<std::string, std::string>& headers,
                 const std::string& body,
                 std::string* body_out,
                 std::vector<std::string>* header_lines,
                 std::string* err) {
  CURL* c = curl_easy_init();
  if (!c) {
    if (err) *err = "curl init failed";
    return 0;
  }
  curl_slist* hdr = nullptr;
  for (auto& [k, v] : headers) hdr = curl_slist_append(hdr, (k + ": " + v).c_str());

  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_POST, 1L);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
  if (hdr) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb_vec);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, body_out);
  if (header_lines) {
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, header_lines);
  }
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);

  return finish(c, hdr, run_transfer(c), err);
}

int http_get_ex(const std::string& url,
                const std::map<std::string, std::string>& headers,
                std::string* body_out,
                std::vector<std::string>* header_lines,
                std::string* err) {
  CURL* c = curl_easy_init();
  if (!c) {
    if (err) *err = "curl init failed";
    return 0;
  }
  curl_slist* hdr = nullptr;
  for (auto& [k, v] : headers) hdr = curl_slist_append(hdr, (k + ": " + v).c_str());
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
  if (hdr) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb_vec);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, body_out);
  if (header_lines) {
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, header_lines);
  }
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);

  return finish(c, hdr, run_transfer(c), err);
}

}  // namespace dog
