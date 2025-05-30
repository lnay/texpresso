/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "sprotocol.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <cstdio>
#include <variant>
#include <optional>

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

query::message query::data::to_enum() {
    return(std::visit(overloaded {
        [](query::open) { return query::message::Q_OPEN; },
        [](query::read) { return query::message::Q_READ; },
        [](query::writ) { return query::message::Q_WRIT; },
        [](query::clos) { return query::message::Q_CLOS; },
        [](query::size) { return query::message::Q_SIZE; },
        [](query::seen) { return query::message::Q_SEEN; },
        [](query::chld) { return query::message::Q_CHLD; },
        [](query::gpic) { return query::message::Q_GPIC; },
        [](query::spic) { return query::message::Q_SPIC; },
    }, *this));
};

answer::message answer::data::to_enum() const {
    return(std::visit(overloaded {
        [](answer::open) { return answer::message::A_OPEN; },
        [](answer::read) { return answer::message::A_READ; },
        [](answer::size) { return answer::message::A_SIZE; },
        [](answer::gpic) { return answer::message::A_GPIC; },
        [](answer::done) { return answer::message::A_DONE; },
        [](answer::pass) { return answer::message::A_PASS; },
        [](answer::fork) { return answer::message::A_FORK; },
    }, *this));
};

const char *query_to_string(query::message q)
{
  switch (q)
  {
    case query::Q_OPEN: return "OPEN";
    case query::Q_READ: return "READ";
    case query::Q_WRIT: return "WRIT";
    case query::Q_CLOS: return "CLOS";
    case query::Q_SIZE: return "SIZE";
    case query::Q_SEEN: return "SEEN";
    case query::Q_GPIC: return "GPIC";
    case query::Q_SPIC: return "SPIC";
    case query::Q_CHLD: return "CHLD";
  }
}

const char *answer_to_string( answer::message q)
{
  switch (q)
  {
    case answer::message::A_DONE: return "DONE";
    case answer::message::A_PASS: return "PASS";
    case answer::message::A_SIZE: return "SIZE";
    case answer::message::A_READ: return "READ";
    case answer::message::A_FORK: return "FORK";
    case answer::message::A_OPEN: return "OPEN";
    case answer::message::A_GPIC: return "GPIC";
  }
}

const char *ask_to_string(enum ask q)
{
  switch (q)
  {
    case C_FLSH: return "FLSH";
  }
}

void query::data::log(FILE *f)
{
  fprintf(f, "%04dms: ", this->time);
  std::visit(overloaded {
      [f](query::open o) {
          fprintf(f, "OPEN(%d, \"%s\", \"%s\")\n", o.fid, o.path, o.mode);
      },
      [f](query::read r) {
          fprintf(f, "READ(%d, %d, %d)\n", r.fid, r.pos, r.size);
      },
      [f](query::writ w) {
          fprintf(f, "WRIT(%d, %d, %d)\n",
              w.fid, w.pos, w.size);
      },
      [f](query::clos c) {
          fprintf(f, "CLOS(%d)\n", c.fid);
      },
      [f](query::size s) {
          fprintf(f, "SIZE(%d)\n", s.fid);
      },
      [f](query::seen s) {
          fprintf(f, "SEEN(%d, %d)\n", s.fid, s.pos);
      },
      [f](query::chld c) {
          fprintf(f, "CHLD(pid:%d, fd:%d)\n", c.pid, c.fd);
      },
      [f](query::gpic g) {
          fprintf(f, "GPIC(\"%s\",%d,%d)\n", g.path, g.type, g.page);
      },
      [f](query::spic s) {
          fprintf(f, "SPIC(\"%s\", %d, %d, %.02f, %.02f, %.02f, %.02f)\n",
                  s.path,
                  s.cache.type, s.cache.page,
                  s.cache.bounds[0], s.cache.bounds[1],
                  s.cache.bounds[2], s.cache.bounds[3]);
      },
  }, *this);
}

static void write_all(const int fd, const char *buf, int size)
{
  while (size > 0)
  {
    int n = write(fd, buf, size);
    if (n == -1)
    {
      if (errno == EINTR) continue;
      perror("sprotocol.c write_all");
      print_backtrace();
      if (errno == ECONNRESET) return;
    }
    if (n <= 0) mabort();

    buf += n;
    size -= n;
  }
}

// Read at most `len` amount of data from file descriptor and write to `buf`
// Return the amount actually read
size_t Channel::read_(const int fd, void *data, const size_t len)
{
  char msg_control[CMSG_SPACE(1 * sizeof(int))] = {0,};
  iovec iov = { .iov_base = data, .iov_len = len };
  msghdr msg = {
    .msg_iov = &iov, .msg_iovlen = 1,
    .msg_controllen = sizeof(msg_control),
  };
  msg.msg_control = &msg_control;

  ssize_t recvd;
  do { recvd = recvmsg(fd, &msg, 0); }
  while (recvd == -1 && errno == EINTR);

  if (recvd == -1)
  {
    if (errno == ECONNRESET)
    {
      fprintf(stderr, "sprotocol:read_: ECONNRESET\n");
      fflush(stderr);
      return 0;
    }
    perror("recvmsg");
    mabort();
  }

  cmsghdr *cm = CMSG_FIRSTHDR(&msg);

  if (cm != nullptr)
  {
    const int *fds0 = (int*)CMSG_DATA(cm);
    int nfds = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);

    if (nfds != 1) abort();

    if (this->passed_fd != -1) abort();
      //close(t->passed_fd);
    this->passed_fd = fds0[0];
  }

  return recvd;
}

// static int buffered_read_at_least(Channel *t, int fd, char *buf, int atleast, int size)
// {
//   int n;
//   char *org = buf, *ok = buf + atleast;
//   if (size < atleast) abort();
//
//   while (1)
//   {
//     n = t->read_(fd, buf, size);
//     if (n == -1)
//       pabort();
//     else if (n == 0)
//       return 0;
//
//     buf += n;
//     size -= n;
//     if (buf >= ok)
//       break;
//   }
//   return (buf - org);
// }

void Channel::set_fd(const file_id fd)
{
  if (fd != this->fd )
  {
    this->fd = fd;
    this->input.pos = 0;
    this->output.pos = 0;
  }
}

// Read at `size` amount of data from file descriptor and write to `buf`
bool Channel::load_size(const int fd, char *buf, ssize_t size)
{
  while (size > 0)
  {
    const size_t n = this->read_(fd, buf, size);
    if (n == 0) return false;

    buf += n;
    size -= n;
  }
  return true;
}

// Flush content of the Channel's output buffer to the file descriptor
void Channel::cflush(const int fd)
{
  const int pos = this->output.pos;
  if (pos == 0) return;
  write_all(fd, this->output.buffer, pos);
  this->output.pos = 0;
}

// Read from file descriptor into the Channel's input buffer until
// there is at least `at_least` content in front of the position `input.pos`.
// This function eagerly reads data at each opportunity
// (which fits within the input buffer).
//
// Modifies `input` struct member
bool Channel::load_at_least(const int fd, const int at_least)
{
  size_t avail = (this->input.len - this->input.pos);
  if (avail >= at_least) return true;

  // Shift backwards discarding already read part of Channel input buffer
  memmove(this->input.buffer, this->input.buffer + this->input.pos, avail);
  this->input.pos = 0;

  // Read from file until enough content is in the input buffer
  while (avail < at_least)
  {
    const ssize_t n = this->read_(fd, this->input.buffer + avail, BUF_SIZE - avail);
    if (n == 0)
    {
      this->input.len = avail;
      return false;
    }
    avail += n;
  }

  this->input.len = avail;
  return true;
}

/* HANDSHAKE */

#define HND_SERVER "TEXPRESSOS01"
#define HND_CLIENT "TEXPRESSOC01"

bool Channel::handshake(const int fd)
{
  char answer[LEN(HND_CLIENT)];
  write_all(fd, HND_SERVER, LEN(HND_SERVER));
  if (!this->load_size(fd, answer, LEN(HND_CLIENT))) return true;
  this->input.len = this->input.pos = 0;
  this->output.pos = 0;
  return (strncmp(HND_CLIENT, answer, LEN(HND_CLIENT)) == 0);
}

/* PROTOCOL DEFINITION */

Channel::Channel()
{
  this->buf = static_cast<char*>(malloc(256));
  if (!this->buf) mabort();
  this->buf_size = 256;
  this->passed_fd = -1;
  this->fd = {};
}

Channel::~Channel()
{
  free(this->buf);
}

// Double the size of the Channel buffer
void Channel::resize_buf()
{
  const size_t new_size = this->buf_size * 2;

  this->buf = static_cast<char*>(realloc(this->buf, new_size));
  if (!this->buf) mabort();
  this->buf_size = new_size;
}

char Channel::cgetc()
{
  if ((this->input.pos == this->input.len) && (!this->load_at_least(this->fd.value(), 1))) return 0;
  return this->input.buffer[this->input.pos++];
}

int Channel::read_zstr(int *pos)
{
  const int p0 = *pos;
  char c;
  do {
    if (*pos == this->buf_size) this->resize_buf();
    c = this->cgetc();
    this->buf[*pos] = c;
    *pos += 1;
  } while (c != 0);
  return p0;
}

bool Channel::read_bytes(size_t pos, size_t size)
{
  while (this->buf_size < pos + size) this->resize_buf();

  const size_t ipos = this->input.pos, ilen = this->input.len;
  if (ipos + size <= ilen)
  {
    memcpy(&this->buf[pos], this->input.buffer + ipos, size);
    this->input.pos += size;
    return 1;
  }

  size_t isize = ilen - ipos;
  memcpy(&this->buf[pos], this->input.buffer + ipos, isize);
  pos += isize;
  size -= isize;
  this->input.pos = this->input.len = 0;
  return this->load_size(this->fd.value(), &this->buf[pos], size);
}

void Channel::write_bytes(const int fd, void *buf, const int size)
{
  if (this->output.pos + size <= BUF_SIZE)
  {
    memcpy(this->output.buffer + this->output.pos, buf, size);
    this->output.pos += size;
    return;
  }

  this->cflush(fd);

  if (size > BUF_SIZE) write_all(fd, (char*) buf, size);
  else
  {
    memcpy(this->output.buffer, buf, size);
    this->output.pos = size;
  }
}

template<typename T> std::optional<T> Channel::try_read_item(const int fd)
{
  if (!this->load_at_least(fd, sizeof(T))) return {};

  uint32_t item;
  memcpy(&item, this->input.buffer + this->input.pos, sizeof(T));
  this->input.pos += sizeof(T);

  return item;
}

template<typename T>
T Channel::read_item(const int fd)
{
  return this->try_read_item<T>(fd).value();
}

template<typename T>
void Channel::write_item(const int fd, T u)
{
  this->write_bytes(fd, &u, sizeof(T));
}

bool Channel::has_pending_query(int timeout) const
{
  if (this->input.pos != this->input.len) return true;

  struct pollfd pfd;
  int n;
  while(true)
  {
    pfd.fd = this->fd.value();
    pfd.events = POLLRDNORM;
    pfd.revents = 0;
    n = poll(&pfd, 1, timeout);
    if (!(n == -1 && errno == EINTR)) break;
  }

  if (n == -1) pabort();
  if (n == 0) return false;
  return true;
}

query::message Channel::peek_query()
{
  uint32_t result = this->read_item<uint32_t>(this->fd.value());
  if (result == 0) abort();
  this->input.pos -= 4;
  return result;
}

std::optional<query::data> Channel::read_query()
{
  const int fd = this->fd.value();
  const auto opt = this->try_read_item<uint32_t>(fd);
  if (!opt.has_value()) return {};
  const uint32_t tag = opt.value();
  const int time = this->read_item<int32_t>(fd);
  int pos = 0;
  static_assert(sizeof(uint32_t) == sizeof(file_id));
  switch (tag)
  {
    case query::Q_OPEN:
    {
        fprintf(stderr, "[info] Reading OPEN");
        const auto fid = this->read_item<file_id>(fd);
        const int pos_path = this->read_zstr(&pos);
        const int pos_mode = this->read_zstr(&pos);

        query::open op = {
            .fid = fid,
            .path = &this->buf[pos_path],
            .mode = &this->buf[pos_mode],
        };
        return query::data(time, op);
    }
    case query::Q_READ:
    {
        fprintf(stderr, "[info] Reading READ");
        return query::data(time, query::read {
            .fid = this->read_item<file_id>(fd),
            .pos = this->read_item<uint32_t>(fd),
            .size = this->read_item<uint32_t>(fd),
        });
    }
    case query::Q_WRIT:
    {
        fprintf(stderr, "[info] Reading WRIT");
        query::writ wr {
            .fid = this->read_item<uint32_t>(fd),
            .pos = this->read_item<uint32_t>(fd),
            .size = this->read_item<uint32_t>(fd),
        };
        if (!this->read_bytes(0, wr.size)) return {};
        wr.buf = this->buf;
        return query::data(time, wr);
    }
    case query::Q_CLOS:
    {
        fprintf(stderr, "[info] Reading CLOS");
        query::clos cl {
            .fid = this->read_item<file_id>(fd)
        };
        return query::data(time, cl);
    }
    case query::Q_SIZE:
    {
        fprintf(stderr, "[info] Reading SIZE");
        query::size si {
            .fid = this->read_item<file_id>(fd)
        };
        return query::data(time, si);
    }
    case query::Q_SEEN:
    {
        fprintf(stderr, "[info] Reading SEEN");
        query::seen se {
            .fid = this->read_item<file_id>(fd),
            .pos = this->read_item<file_id>(fd),
        };
        return query::data(time, se);
    }
    case query::Q_GPIC:
    {
        fprintf(stderr, "[info] Reading GPIC");
        int pos_path = this->read_zstr(&pos);
        query::gpic gp {
            .path = &this->buf[pos_path],
            .type = this->read_item<file_id>(fd),
            .page = this->read_item<file_id>(fd),
        };
        return query::data(time, gp);
    }
    case query::Q_SPIC:
    {
        fprintf(stderr, "[info] Reading SPIC");
        int pos_path = this->read_zstr(&pos);
        query::spic sp {
            .path = &this->buf[pos_path],
            .cache = {
                .type = this->read_item<file_id>(fd),
                .page = this->read_item<file_id>(fd),
                .bounds = {
                    this->read_item<float>(fd),
                    this->read_item<float>(fd),
                    this->read_item<float>(fd),
                    this->read_item<float>(fd),
                }
            }
        };
        return query::data(time, sp);
    }
    case query::Q_CHLD:
    {
        fprintf(stderr, "[info] Reading CHLD");
        query::chld ch {
            .pid = static_cast<file_id>(this->read_item<uint32_t>(fd)),
            .fd = this->passed_fd,
        };
        if (ch.fd == -1) abort();
        this->passed_fd = -1;
        return query::data(time, ch);
    }
    default:
    {
        fprintf(stderr, "unexpected tag: %c%c%c%c\n",
                tag & 0xFF, (tag >> 8) & 0xFF, (tag >> 16) & 0xFF, (tag >> 24) & 0xFF);
        mabort();
        return {};
    }
  }
  // put log on the caller?
  // if (LOG)
  // {
  //   fprintf(stderr, "[info] <- ");
  //   log_query(stderr, r);
  // }
}

void Channel::write_ask(const int fd, ask_t *a)
{
  this->write_item(fd, a->tag);
  switch (a->tag)
  {
    case C_FLSH: break;
    default: mabort();
  }
}

// static void write_time(Channel *t, int fd, struct stat_time tm)
// {
//   write_u32(t, fd, tm.sec);
//   write_u32(t, fd, tm.nsec);
// }

void Channel::write_answer(const int fd, const answer::data &a)
{
    // if (LOG)
    // {
      // if (a->tag == READ)
      //   fprintf(stderr, "[info] -> READ %d\n", a->read.size);
      // else
        fprintf(stderr, "[info] -> %s\n", answer_to_string(a.to_enum()));
    // }
    this->write_item(fd, static_cast<uint32_t>(a.to_enum()));
    std::visit(overloaded {
        [](answer::done _) {},
        [](answer::pass _) {},
        [](answer::fork _) {},
        [fd, this](answer::read r) {
            this->write_item(fd, r.size);
            this->write_bytes(fd, this->buf, r.size);
        },
        [fd, this](answer::size s) {
            this->write_item(fd, s.size);
        },
        [fd, this](answer::open o) {
            this->write_item(fd, o.size);
            this->write_bytes(fd, this->buf, o.size);
        },
        [fd, this](answer::gpic g) {
            this->write_item(fd, g.bounds[0]);
            this->write_item(fd, g.bounds[1]);
            this->write_item(fd, g.bounds[2]);
            this->write_item(fd, g.bounds[3]);
        },
    }, a);
}

void Channel::flush(const int fd)
{
  this->cflush(fd);
}

void Channel::reset()
{
  this->input.pos = this->input.len = 0;
  this->output.pos = 0;
}

void *Channel::get_buffer(const size_t n)
{
  while (n > this->buf_size) this->resize_buf();
  return this->buf;
}
