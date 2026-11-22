// The link client against replies a well-behaved safety MCU never sends: found by the link fuzzer
// (stm32/tests/fuzz_link.cpp). A PARAM_HELP piece whose total is below the offset asked for (the
// MCU's text got shorter between two pieces, or a garbled reply) leaves the description empty
// instead of reading far past the reply.
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

#include "../link_client.h"
#include "check.h"

using esphome::flamekeeper::LinkClient;
using esphome::flamekeeper::LinkTransport;

// An MCU with one parameter whose description shrinks to 10 bytes after its first piece.
struct ShrinkingHelp : LinkTransport {
  std::deque<uint8_t> rx;
  int help_asks = 0;
  void send(const uint8_t *f, size_t) override {
    uint8_t out[LINK_REPLY_PAYLOAD_MAX], reply[LINK_FRAME_MAX];
    size_t len = 0;
    const uint8_t *q = link_request_payload(f);
    switch (link_frame_op(f)) {
      case LINK_OP_HELLO: {
        link_info info{};
        info.major = LINK_MAJOR;
        info.kind = LINK_KIND_EMULATOR;
        info.capabilities = LINK_CAP_PARAMETERS;
        info.param_count = 1;
        len = link_encode_info(&info, out);
        break;
      }
      case LINK_OP_POLL: {
        link_status st;
        link_status_unknown(&st);
        st.state = 1;
        len = link_encode_status(&st, out);
        break;
      }
      case LINK_OP_PARAM_DESCRIBE: {
        link_param_desc d{};
        d.count = 1;
        d.id = 7;
        d.max = 100;
        std::strcpy(d.name, "Preheat time");
        len = link_encode_param_desc(&d, out);
        break;
      }
      case LINK_OP_PARAM_HELP: {
        help_asks++;
        out[0] = q[0], out[1] = q[1], out[2] = q[2];
        out[3] = q[2] == 0 ? 100 : 10;  // 100 bytes long, then only 10
        size_t chunk = q[2] == 0 ? LINK_PARAM_HELP_CHUNK : 0;
        std::memset(out + LINK_PARAM_HELP_FIXED, 'a', chunk);
        len = LINK_PARAM_HELP_FIXED + chunk;
        break;
      }
      default:
        break;
    }
    size_t n = link_build_reply(link_frame_seq(f), link_frame_op(f), LINK_OK, out, len, reply);
    rx.insert(rx.end(), reply, reply + n);
  }
  int read() override {
    if (rx.empty())
      return -1;
    int b = rx.front();
    rx.pop_front();
    return b;
  }
};

static void test_help_total_below_offset() {
  ShrinkingHelp mcu;
  LinkClient client(&mcu);
  for (uint32_t t = 0; t < 5000; t += 10)
    client.loop(t);
  CHECK(client.up());
  CHECK(client.param_count() == 1);
  CHECK(mcu.help_asks == 2);
  CHECK(std::strlen(client.param_help(7)) == 0);  // left without one: a muddle
}

int main() {
  test_help_total_below_offset();
  return report("link client replies");
}
