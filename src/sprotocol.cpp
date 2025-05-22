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
#include <array>

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

bool Channel::Input::load_()
{
  const file_id fd = this->fd;
  char msg_control[CMSG_SPACE(1 * sizeof(int))] = {0,};
  iovec iov = { .iov_base = this->buffer + this->len, .iov_len = BUF_SIZE - this->len};
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
    const int nfds = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);

    if (nfds != 1) abort();

    if (this->passed_fd != -1) abort();
    //close(t->passed_fd);
    this->passed_fd = fds0[0];
  }

  this->len += recvd;
  return recvd != 0;
}

void Channel::Input::empty()
{
  this->pos = 0;
  this->len = 0;
}

char Channel::Input::getc()
{
  if ((this->pos == this->len) && (!this->load_at_least(1))) return 0;
  return this->buffer[this->pos++];
}

void Channel::Input::trim_read()
{
  size_t avail = (this->len - this->pos);
  memmove(this->buffer, this->buffer + this->pos, avail);
  this->pos = 0;
  this->len = avail;
}

// Load at least `at_least` bytes of data from `fd` ahead of the
// position `pos`, trimming already read data if necessary to make
// space
bool Channel::Input::load_at_least(const size_t at_least)
{
  if (this->len >= this->pos + at_least) return true;

  // Drop already read buffer content if not enough space in remainder of
  // buffer
  if (BUF_SIZE < this->pos + at_least ) this->trim_read();

  // Read from file until enough content is in the input buffer
  while (this->len < this->pos + at_least)
  {
    if (!this->load_()) return false;
  }
  return true;
}

template<typename T>
std::optional<T> Channel::Input::read_item()
{
  // this could be optimised like it was before to load_ straight into
  // `out` if the necessary content is not all currently already loaded
  // into the input buffer
  if (!this->load_at_least(sizeof(T))) return {};
  T out = *((T*)(this->buffer + this->pos));
  this->pos += sizeof(T);
  return out;
}

template<typename T>
std::optional<T> Channel::Input::peek_item()
{
  // this could be optimised like it was before to load_ straight into
  // `out` if the necessary content is not all currently already loaded
  // into the input buffer
  if (!this->load_at_least(sizeof(T))) return {};
  return *((T*)(this->buffer + this->pos));
}

// Read at most `len` amount of data from file descriptor and write to `buf`
// Return the amount actually read
size_t Channel::load_(void *data, const size_t len)
{
  const file_id fd = this->fd.value();
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

void Channel::set_fd(const file_id fd)
{
  if (fd != this->fd )
  {
    this->fd = fd;
    this->input.pos = 0;
    this->output.pos = 0;
    this->input.fd = fd;
    this->output.fd = fd;
  }
}

// Read at `size` amount of data from file descriptor and write to `buf`
bool Channel::load_size(char *buf, ssize_t size)
{
  while (size > 0)
  {
    const size_t n = this->load_(buf, size);
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

/* HANDSHAKE */

#define HND_SERVER "TEXPRESSOS01"
#define HND_CLIENT "TEXPRESSOC01"

bool Channel::handshake()
{
  write_all(this->fd.value(), HND_SERVER, LEN(HND_SERVER));
  this->input.empty();
  auto answer = this->input.read_item<std::array<char, 12>>();
  this->output.pos = 0;
  return (strncmp(HND_CLIENT, answer.value().data(), LEN(HND_CLIENT)) == 0);
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

// Move position on buf forwards past the zero terminated string and return
// the position of the string just moved past (copy of the initial pos)
char* Channel::read_zstr()
{
  char* out = this->buf + this->buf_pos;
  char c;
  do {
    if (this->buf_pos == this->buf_size) this->resize_buf();
    c = this->input.getc();
    this->buf[this->buf_pos++] = c;
  } while (c != NULL);
  return out;
}

std::optional<char*> Channel::read_bytes(size_t size)
{
  char* out = this->buf + this->buf_pos;
  while (this->buf_size < this->buf_pos + size) this->resize_buf();

  const size_t ipos = this->input.pos;
  const size_t ilen = this->input.len;
  if (ipos + size <= ilen)
  {
    // there is already enough bytes loaded into the input buffer
    memcpy(this->buf + this->buf_pos, this->input.buffer + ipos, size);
    this->input.pos += size;
    return out;
  }

  // there is already enough bytes loaded into the input buffer
  size_t isize = ilen - ipos;
  memcpy(this->buf + this->buf_pos, this->input.buffer + ipos, isize);
  this->buf_pos += isize;
  size -= isize;
  this->input.pos = this->input.len = 0;
  if (!this->load_size(this->buf + this->buf_pos, size)) return {};
  return out;
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
  return this->input.peek_item<query::message>().value();
}

std::optional<query::data> Channel::read_query()
{
  const auto opt = this->input.read_item<uint32_t>();
  if (!opt.has_value()) return {};
  const uint32_t tag = opt.value();
  const int time = this->input.read_item<int32_t>().value();
  this->buf_pos = 0;
  static_assert(sizeof(uint32_t) == sizeof(file_id));
  switch (tag)
  {
    case query::Q_OPEN:
    {
        fprintf(stderr, "[info] Reading OPEN");
        query::open op = {
            .fid = this->input.read_item<file_id>().value(),
            .path = this->read_zstr(),
            .mode = this->read_zstr(),
        };
        return query::data(time, op);
    }
    case query::Q_READ:
    {
        fprintf(stderr, "[info] Reading READ");
        return query::data(time, query::read {
            .fid = this->input.read_item<file_id>().value(),
            .pos = this->input.read_item<int32_t>().value(),
            .size = this->input.read_item<int32_t>().value(),
        });
    }
    case query::Q_WRIT:
    {
      fprintf(stderr, "[info] Reading WRIT");
      try
      {
        int32_t size;
        query::writ wr{
          .fid = this->input.read_item<file_id>().value(),
          .pos = this->input.read_item<int32_t>().value(),
          .size = size = this->input.read_item<int32_t>().value(),
          .buf = this->read_bytes(size).value(),
        };
        return query::data(time, wr);
      } catch (...)
      {
        return {};
      }
    }
    case query::Q_CLOS:
    {
        fprintf(stderr, "[info] Reading CLOS");
        query::clos cl {
            .fid = this->input.read_item<file_id>().value(),
        };
        return query::data(time, cl);
    }
    case query::Q_SIZE:
    {
        fprintf(stderr, "[info] Reading SIZE");
        query::size si {
            .fid = this->input.read_item<file_id>().value(),
        };
        return query::data(time, si);
    }
    case query::Q_SEEN:
    {
        fprintf(stderr, "[info] Reading SEEN");
        query::seen se {
            .fid = this->input.read_item<file_id>().value(),
            .pos = this->input.read_item<file_id>().value(),
        };
        return query::data(time, se);
    }
    case query::Q_GPIC:
    {
        fprintf(stderr, "[info] Reading GPIC");
        query::gpic gp {
            .path = this->read_zstr(),
            .type = this->input.read_item<file_id>().value(),
            .page = this->input.read_item<file_id>().value(),
        };
        return query::data(time, gp);
    }
    case query::Q_SPIC:
    {
        fprintf(stderr, "[info] Reading SPIC");
        query::spic sp {
            .path = this->read_zstr(),
            .cache = {
                .type = this->input.read_item<file_id>().value(),
                .page = this->input.read_item<file_id>().value(),
                .bounds = {
                    this->input.read_item<float>().value(),
                    this->input.read_item<float>().value(),
                    this->input.read_item<float>().value(),
                    this->input.read_item<float>().value(),
                }
            }
        };
        return query::data(time, sp);
    }
    case query::Q_CHLD:
    {
        fprintf(stderr, "[info] Reading CHLD");
        query::chld ch {
            .pid = this->input.read_item<file_id>().value(),
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
