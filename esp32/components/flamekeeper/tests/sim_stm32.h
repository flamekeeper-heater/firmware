// A simulated STM32G030K8 safety MCU for the host tests, at the far end of the link UART and the
// reset and boot lines:
//
// - The ROM bootloader on USART2 at 8E1 (ST AN3155): the 0x7F sync, ACK / NACK, Get, Get ID, Read
//   Memory, Write Memory, Extended Erase and Go, with their checksums and NACKs (bad complement or
//   checksum, an invalid address, read-out protection, programming a double word that is not
//   erased). It waits for every byte as long as it takes, as the real one does.
// - 64 KB of flash in 2 KB pages, erased to 0xFF; the option bytes at 0x1FFF7800 (value and
//   complement per double word), loaded at power-on and after an option write; FLASH_OPTR's boot
//   bits, the BOOT0 pin and the EMPTY flag (updated only by an option load) choose what runs after
//   a reset. A write to the option bytes is ACKed, then resets the chip with an option load.
// - An application: when the vector table is plausible, a link server (8N1) whose HELLO reports the
//   version in flash at 0x100 (this simulation's convention) and that agrees to UPDATE_PREPARE
//   unless the heater is "hot". With an erased vector table the core locks up: silent.
// - Faults: a lost byte either way, a NACK, a hang until the next reset, a write that programs a
//   wrong bit (once, or always in one page), and a power loss halfway through a write.
//
// Bytes sent with the other framing (8N1 to the bootloader, 8E1 to the application) are lost.
#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>

#include "../link.h"
#include "../stm32_boot.h"

struct SimStm32 : public esphome::flamekeeper::BootPort {
  using Boot = esphome::flamekeeper::Stm32Boot;
  static const uint32_t FLASH_SIZE = 64 * 1024;
  static const uint32_t FLASH_BASE = 0x08000000;
  static const uint32_t PAGE = 2048;
  static const uint32_t OPT_BASE = 0x1FFF7800;
  static const uint32_t OPT_SIZE = 128;
  static const uint32_t FACTORY_OPTR = 0xDFFFE1AAu;  // WDG-15
  static const uint32_t ERASE_MS = 25;  // per page
  static const uint32_t WRITE_MS = 3;   // per 256 bytes

  enum Mode { RESET, BOOTLOADER, APP, LOCKUP };

  // ---- the chip
  uint8_t flash[FLASH_SIZE];
  uint8_t opt[OPT_SIZE];
  uint32_t optr = FACTORY_OPTR;  // FLASH_OPTR as loaded
  bool optverr = false;
  bool empty = true;  // the EMPTY flag, from the last option load
  uint16_t pid = 0x466;
  bool powered = true;
  Mode mode = RESET;

  // ---- the lines, as the ESP32 drives them
  bool host_8e1 = false;
  bool boot_high = false;
  bool reset_low = false;
  int boot_drives = 0, reset_drives = 0;  // times the ESP32 drove each line

  // ---- what happened
  bool saved_touched = false;  // pages 30 or 31 erased or written
  bool mass_erased = false;
  int erases = 0, writes = 0, option_writes = 0, gos = 0, resets = 0, prepares = 0, entries = 0;
  int hellos = 0;
  std::vector<uint32_t> write_log;  // flash addresses written, in order
  std::vector<uint16_t> erase_log;  // pages erased, in order

  // ---- faults (counts from 1; 0 = none)
  int drop_in_at = 0;          // the nth byte the bootloader receives is lost
  int drop_out_at = 0;         // the nth byte the bootloader sends is lost
  int nack_write_at = 0;       // the nth Write Memory is NACKed after its data
  int corrupt_write_at = 0;    // the nth Write Memory programs one wrong bit (and is ACKed)
  int bad_page = -1;           // every write into this page programs one wrong bit
  int power_loss_write_at = 0; // the power fails during the nth Write Memory, half of it programmed
  int hang_at_command = 0;     // the bootloader stops answering at the nth command, until a reset
  bool go_nack = false;        // Go is NACKed at its address
  bool app_hot = false;        // the application refuses UPDATE_PREPARE (the heater is not cold)
  uint8_t app_state = 1;       // the heater state the application reports (1 off)
  bool option_write_ignored = false;  // the bootloader ACKs an option write but changes nothing
  int in_count = 0, out_count = 0, commands = 0, write_count = 0;

  uint32_t now = 0;

  SimStm32() {
    std::memset(this->flash, 0xFF, sizeof(this->flash));
    this->set_optr(FACTORY_OPTR);
  }

  // ---------------------------------------------------------------- set-up for the tests

  void set_optr(uint32_t value) {
    std::memset(this->opt, 0xFF, sizeof(this->opt));
    put_le(this->opt, value);
    put_le(this->opt + 4, ~value);
    for (uint32_t i = 8; i < OPT_SIZE; i += 8) {  // the other option words, at their reset values
      put_le(this->opt + i, 0x000000FFu);
      put_le(this->opt + i + 4, ~0x000000FFu);
    }
    this->option_load();
  }
  // An application of the given version, size bytes (the vector table and the version at 0x100).
  void program_app(const std::vector<uint8_t> &image) {
    for (uint32_t i = 0; i < 30 * PAGE; i++)
      this->flash[i] = i < image.size() ? image[i] : 0xFF;
  }
  void fill_saved_pages() {
    for (uint32_t i = 30 * PAGE; i < FLASH_SIZE; i++)
      this->flash[i] = uint8_t(0xA5 ^ i);
  }
  bool saved_pages_intact() const {
    for (uint32_t i = 30 * PAGE; i < FLASH_SIZE; i++)
      if (this->flash[i] != uint8_t(0xA5 ^ i))
        return false;
    return true;
  }
  void power_on() {
    this->powered = true;
    this->option_load();
    this->boot();
  }
  void power_off() {
    this->powered = false;
    this->mode = RESET;
    this->out.clear();
    this->delayed.clear();
  }

  // ---------------------------------------------------------------- BootPort (the ESP32's side)

  void send(const uint8_t *data, size_t len) override {
    for (size_t i = 0; i < len; i++) {
      if (!this->powered || this->reset_low)
        continue;
      if (this->mode == BOOTLOADER && this->host_8e1)
        this->boot_rx(data[i]);
      else if (this->mode == APP && !this->host_8e1)
        this->app_rx(data[i]);
    }
  }
  int read() override {
    while (!this->out.empty()) {
      auto b = this->out.front();
      this->out.pop_front();
      if (b.second == this->host_8e1)
        return b.first;
    }
    return -1;
  }
  void bootloader_framing(bool on) override { this->host_8e1 = on; }
  void drive_boot(bool high) override {
    if (high)
      this->boot_drives++;
    this->boot_high = high;
  }
  void drive_reset(bool low) override {
    if (low) {
      this->reset_drives++;
      this->reset_low = true;
      this->mode = RESET;
      this->out.clear();
      this->delayed.clear();
    } else if (this->reset_low) {
      this->reset_low = false;
      if (this->powered) {
        this->resets++;
        this->boot();
      }
    }
  }

  // Delayed replies (an erase or a write in progress) go out once their time has come.
  void tick(uint32_t t) {
    this->now = t;
    if (!this->delayed.empty() && int32_t(t - this->busy_until) >= 0) {
      std::vector<uint8_t> d;
      d.swap(this->delayed);
      for (uint8_t b : d)
        this->emit(b);
      if (this->reset_after_reply) {
        this->reset_after_reply = false;
        this->option_load();  // the bootloader's reset after an option write
        this->boot();
      }
    }
  }

  // ---------------------------------------------------------------- the chip

  static void put_le(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
  }
  static uint32_t get_le(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
  }

  // WDG-7: a word whose complement does not match loads every USR bit as 1 and RDP as level 1.
  void option_load() {
    uint32_t v = get_le(this->opt), c = get_le(this->opt + 4);
    this->optverr = v != ~c;
    this->optr = this->optverr ? 0xFFFFFFFFu : v;
    this->empty = get_le(this->flash) == 0xFFFFFFFFu;
  }
  bool rdp_level0() const { return (this->optr & 0xFF) == 0xAA; }

  void boot() {
    this->synced = false;
    this->hung = false;
    this->in.clear();
    this->stage = 0;
    this->reset_after_reply = false;
    this->out.clear();
    this->delayed.clear();
    this->no_starts = false;
    this->client_major = 0;
    link_parser_init(&this->parser, LINK_REQUEST);
    const bool nboot_sel = this->optr & (1u << 24), nboot1 = this->optr & (1u << 25), nboot0 = this->optr & (1u << 26);
    const bool boot0 = nboot_sel ? !nboot0 : this->boot_high;
    if (boot0)
      this->mode = nboot1 ? BOOTLOADER : LOCKUP;  // system memory, or SRAM (nothing there)
    else if (this->empty)
      this->mode = BOOTLOADER;
    else
      this->start_app(FLASH_BASE);
    if (this->mode == BOOTLOADER)
      this->entries++;
  }
  void start_app(uint32_t address) {
    uint32_t off = address - FLASH_BASE;
    uint32_t sp = get_le(this->flash + off), pc = get_le(this->flash + off + 4);
    bool ok = sp > 0x20000000u && sp <= 0x20002000u && (pc & 1) && pc > address && pc < FLASH_BASE + FLASH_SIZE;
    this->mode = ok ? APP : LOCKUP;
  }

  // ---------------------------------------------------------------- the bootloader

  std::deque<std::pair<uint8_t, bool>> out;  // byte, sent at 8E1
  std::vector<uint8_t> delayed;
  uint32_t busy_until = 0;
  bool reset_after_reply = false;
  bool synced = false, hung = false;
  std::vector<uint8_t> in;
  uint8_t cmd = 0;
  int stage = 0;
  uint32_t address = 0;

  void emit(uint8_t b) {
    if (this->mode == BOOTLOADER && ++this->out_count == this->drop_out_at)
      return;
    this->out.push_back({b, this->mode == BOOTLOADER});
  }
  void reply(std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes)
      this->emit(b);
  }
  void later(uint32_t ms, uint8_t b) {
    this->busy_until = this->now + ms;
    this->delayed.push_back(b);
  }
  void nack() {
    this->emit(Boot::NACK);
    this->idle();
  }
  void idle() {
    this->in.clear();
    this->stage = 0;
  }
  static uint8_t xor_of(const uint8_t *p, size_t n) {
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++)
      x ^= p[i];
    return x;
  }
  bool readable(uint32_t a, uint32_t n) const {
    return (a >= FLASH_BASE && a + n <= FLASH_BASE + FLASH_SIZE) || (a >= OPT_BASE && a + n <= OPT_BASE + OPT_SIZE) ||
           (a >= 0x20000000u && a + n <= 0x20002000u) || (a >= 0x1FFF0000u && a + n <= 0x1FFF2000u);
  }

  void boot_rx(uint8_t b) {
    if (this->hung)
      return;
    if (++this->in_count == this->drop_in_at)
      return;
    if (!this->synced) {
      if (b == Boot::SYNC) {
        this->synced = true;
        this->emit(Boot::ACK);
      }
      return;  // anything else before the sync: ignored
    }
    this->in.push_back(b);
    this->boot_step();
  }

  void boot_step() {
    std::vector<uint8_t> &v = this->in;
    switch (this->stage) {
      case 0: {  // the command and its complement
        if (v.size() < 2)
          return;
        this->cmd = v[0];
        if (uint8_t(v[0] ^ v[1]) != 0xFF)
          return this->nack();
        this->in.clear();
        if (++this->commands == this->hang_at_command) {
          this->hung = true;
          return;
        }
        switch (this->cmd) {
          case Boot::GET:  // the version, then the commands it takes
            this->reply({Boot::ACK, 11, 0x53, Boot::GET, 0x01, Boot::GET_ID, Boot::READ, Boot::GO, Boot::WRITE,
                         Boot::EXT_ERASE, 0x63, 0x73, 0x82, 0x92, Boot::ACK});
            return this->idle();
          case Boot::GET_ID:
            this->reply({Boot::ACK, 0x01, uint8_t(this->pid >> 8), uint8_t(this->pid), Boot::ACK});
            return this->idle();
          case Boot::READ:
          case Boot::WRITE:
          case Boot::EXT_ERASE:
            if (!this->rdp_level0())
              return this->nack();
            this->emit(Boot::ACK);
            this->stage = 1;
            return;
          case Boot::GO:
            this->emit(Boot::ACK);
            this->stage = 1;
            return;
          default:
            return this->nack();
        }
      }
      case 1:
        if (this->cmd == Boot::EXT_ERASE) {  // the page count, then the pages or a special erase
          if (v.size() < 2)
            return;
          this->stage = 2;
          return this->boot_step();
        }
        if (v.size() < 5)
          return;
        if (xor_of(v.data(), 4) != v[4])
          return this->nack();
        this->address = uint32_t(v[0]) << 24 | uint32_t(v[1]) << 16 | uint32_t(v[2]) << 8 | v[3];
        this->in.clear();
        if (this->cmd == Boot::GO)
          return this->go();
        if (!this->readable(this->address, 1) || (this->cmd == Boot::WRITE && (this->address & 3)))
          return this->nack();
        this->emit(Boot::ACK);
        this->stage = 2;
        return;
      case 2:
        if (this->cmd == Boot::READ) {
          if (v.size() < 2)
            return;
          if (uint8_t(v[0] ^ v[1]) != 0xFF || !this->readable(this->address, uint32_t(v[0]) + 1))
            return this->nack();
          const uint32_t n = uint32_t(v[0]) + 1;
          this->emit(Boot::ACK);
          for (uint32_t i = 0; i < n; i++)
            this->emit(this->peek(this->address + i));
          return this->idle();
        }
        if (this->cmd == Boot::WRITE) {
          if (v.empty() || v.size() < size_t(v[0]) + 3)
            return;
          if (xor_of(v.data(), v.size() - 1) != v.back())
            return this->nack();
          const std::vector<uint8_t> data(v.begin() + 1, v.end() - 1);
          return this->write(uint32_t(data.size()), data.data());
        }
        // Extended Erase
        {
          const uint16_t n = uint16_t(v[0] << 8 | v[1]);
          if (n >= 0xFFF0) {  // mass or bank erase: the count, then a checksum
            if (v.size() < 3)
              return;
            if (uint8_t(v[0] ^ v[1]) != v[2])
              return this->nack();
            std::memset(this->flash, 0xFF, sizeof(this->flash));
            this->mass_erased = this->saved_touched = true;
            this->later(ERASE_MS * 32, Boot::ACK);
            return this->idle();
          }
          const size_t need = 2 + 2 * (size_t(n) + 1) + 1;
          if (v.size() < need)
            return;
          if (xor_of(v.data(), need - 1) != v[need - 1])
            return this->nack();
          for (size_t i = 0; i <= n; i++) {
            uint16_t page = uint16_t(v[2 + 2 * i] << 8 | v[3 + 2 * i]);
            if (page >= FLASH_SIZE / PAGE)
              return this->nack();
          }
          for (size_t i = 0; i <= n; i++) {
            uint16_t page = uint16_t(v[2 + 2 * i] << 8 | v[3 + 2 * i]);
            std::memset(this->flash + page * PAGE, 0xFF, PAGE);
            this->erase_log.push_back(page);
            if (page >= 30)
              this->saved_touched = true;
            this->erases++;
          }
          this->later(ERASE_MS * (n + 1u), Boot::ACK);
          return this->idle();
        }
      default:
        return;
    }
  }

  uint8_t peek(uint32_t a) const {
    if (a >= FLASH_BASE && a < FLASH_BASE + FLASH_SIZE)
      return this->flash[a - FLASH_BASE];
    if (a >= OPT_BASE && a < OPT_BASE + OPT_SIZE)
      return this->opt[a - OPT_BASE];
    return 0;
  }

  void write(uint32_t n, const uint8_t *data) {
    const uint32_t a = this->address;
    this->idle();
    this->write_count++;
    if (a == OPT_BASE) {  // the option bytes: all erased, then written, then the chip resets
      this->option_writes++;
      if (n > OPT_SIZE)
        return this->nack();
      if (!this->option_write_ignored) {
        std::memset(this->opt, 0xFF, OPT_SIZE);
        std::memcpy(this->opt, data, n);
      }
      this->later(WRITE_MS, Boot::ACK);
      this->reset_after_reply = true;
      return;
    }
    if (a >= 0x20000000u) {  // RAM
      this->emit(Boot::ACK);
      return;
    }
    if (a < FLASH_BASE || a + n > FLASH_BASE + FLASH_SIZE || (a & 7) || (n & 7))
      return this->nack();
    if (this->write_count == this->nack_write_at)
      return this->nack();
    const uint32_t off = a - FLASH_BASE;
    for (uint32_t i = 0; i < n; i++)
      if (this->flash[off + i] != 0xFF)
        return this->nack();  // PROGERR: the double word is not erased
    uint32_t done = n;
    if (this->write_count == this->power_loss_write_at)
      done = n / 2;
    std::memcpy(this->flash + off, data, done);
    this->write_log.push_back(a);
    if ((off / PAGE) >= 30)
      this->saved_touched = true;
    this->writes++;
    if (this->write_count == this->power_loss_write_at)
      return this->power_off();
    if (this->write_count == this->corrupt_write_at || int(off / PAGE) == this->bad_page)
      this->flash[off + n / 2] ^= 0x10;
    this->later(WRITE_MS, Boot::ACK);
  }

  void go() {
    if (this->go_nack || !(this->address >= FLASH_BASE && this->address < FLASH_BASE + FLASH_SIZE))
      return this->nack();
    this->emit(Boot::ACK);
    this->gos++;
    this->idle();
    this->synced = false;
    link_parser_init(&this->parser, LINK_REQUEST);
    this->no_starts = false;
    this->client_major = 0;
    this->start_app(this->address);
  }

  // ---------------------------------------------------------------- the application

  link_parser parser{};
  uint8_t client_major = 0;
  bool no_starts = false;

  static void on_info(void *ctx, link_info *o) {
    SimStm32 *s = static_cast<SimStm32 *>(ctx);
    s->hellos++;
    o->kind = LINK_KIND_STM32;
    std::memcpy(o->version, s->flash + 0x100, 3);
    o->capabilities = LINK_CAP_FAN_ONLY;
    o->param_count = 0;
    std::strcpy(o->text, "sim safety");
  }
  static void on_poll(void *ctx, const link_desired *want, link_status *o) {
    (void) want;
    SimStm32 *s = static_cast<SimStm32 *>(ctx);
    link_status_unknown(o);
    o->state = s->app_state;
    o->heat_exchanger = s->app_hot ? 900 : 200;
    o->flags = s->no_starts ? LINK_STATUS_NO_STARTS : 0;
  }
  static int on_prepare(void *ctx, uint8_t *method) {
    SimStm32 *s = static_cast<SimStm32 *>(ctx);
    s->prepares++;
    if (s->app_hot)
      return LINK_NOT_OFF;
    s->no_starts = true;
    *method = 1;
    return LINK_OK;
  }
  void app_rx(uint8_t b) {
    if (!link_parser_feed(&this->parser, b))
      return;
    link_mcu m{};
    m.ctx = this;
    m.info = on_info;
    m.poll = on_poll;
    m.update_prepare = on_prepare;
    m.client_major = &this->client_major;
    uint8_t reply[LINK_FRAME_MAX];
    size_t n = link_server_handle(&m, this->parser.buf, reply);
    for (size_t i = 0; i < n; i++)
      this->emit(reply[i]);
  }
};
