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

#include "sprotocol.hpp"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <cstdint>
#include <variant>
#include <optional>

#define PACK(a,b,c,d) ((d << 24) | (c << 16) | (b << 8) | a)

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

query::message query::data::to_enum() {
    return(std::visit(overloaded {
        [](query::open) { return query::message::OPEN; },
        [](query::read) { return query::message::READ; },
        [](query::writ) { return query::message::WRIT; },
        [](query::clos) { return query::message::CLOS; },
        [](query::size) { return query::message::SIZE; },
        [](query::seen) { return query::message::SEEN; },
        [](query::chld) { return query::message::CHLD; },
        [](query::gpic) { return query::message::GPIC; },
        [](query::spic) { return query::message::SPIC; },
    }, *this));
};

ssize_t Channel::read_(int fd, void *data, size_t len)
{
  char msg_control[CMSG_SPACE(1 * sizeof(int))] = {0,};
  int32_t pid;
  struct iovec iov = { .iov_base = data, .iov_len = len };
  struct msghdr msg = {
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

  struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);

  if (cm != NULL)
  {
    int *fds0 = (int*)CMSG_DATA(cm);
    int nfds = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);

    if (nfds != 1)
      abort();

    if (this->passed_fd != -1)
      abort();
      //close(t->passed_fd);
    this->passed_fd = fds0[0];
  }

  return recvd;
}

int Channel::buffered_read_at_least(int fd, char *buf, int atleast, int size)
{
  int n;
  char *org = buf, *ok = buf + atleast;
  if (size < atleast) abort();

  while (1)
  {
    n = this->read_(fd, buf, size);
    if (n == -1)
      pabort();
    else if (n == 0)
      return 0;

    buf += n;
    size -= n;
    if (buf >= ok)
      break;
  }
  return (buf - org);
}

bool Channel::read_all(int fd, char *buf, int size)
{
  while (size > 0)
  {
    int n = this->read_(fd, buf, size);
    if (n == 0)
      return 0;

    buf += n;
    size -= n;
  }
  return 1;
}

static void write_all(int fd, const char *buf, int size)
{
  while (size > 0)
  {
    int n = write(fd, buf, size);
    if (n == -1)
    {
      if (errno == EINTR)
        continue;
      perror("sprotocol.c write_all");
      print_backtrace();
      if (errno == ECONNRESET)
        return;
    }
    if (n <= 0)
      mabort();

    buf += n;
    size -= n;
  }
}

void Channel::cflush(int fd)
{
  int pos = this->output.pos;
  if (pos == 0) return;
  write_all(fd, this->output.buffer, pos);
  this->output.pos = 0;
}

bool Channel::refill_at_least(int fd, int at_least)
{
  int avail = (this->input.len - this->input.pos);
  if (avail >= at_least)
    return 1;

  memmove(this->input.buffer, this->input.buffer + this->input.pos, avail);

  this->input.pos = 0;
  while (avail < at_least)
  {
    int n = this->read_(fd, this->input.buffer + avail, BUF_SIZE - avail);
    if (n == 0)
    {
      this->input.len = avail;
      return 0;
    }
    avail += n;
  }

  this->input.len = avail;
  return 1;
}

/* HANDSHAKE */

#define HND_SERVER "TEXPRESSOS01"
#define HND_CLIENT "TEXPRESSOC01"

bool Channel::handshake(int fd)
{
  char answer[LEN(HND_CLIENT)];
  write_all(fd, HND_SERVER, LEN(HND_SERVER));
  if (!this->read_all(fd, answer, LEN(HND_CLIENT)))
    return 0;
  this->input.len = this->input.pos = 0;
  this->output.pos = 0;
  return (strncmp(HND_CLIENT, answer, LEN(HND_CLIENT)) == 0);
}

/* PROTOCOL DEFINITION */

const char *query_to_string(enum query::message q)
{
  switch (q)
  {
    case query ::message::OPEN:
      return "OPEN";
    case query ::message::READ:
      return "READ";
    case query ::message::WRIT:
      return "WRIT";
    case query ::message::CLOS:
      return "CLOS";
    case query ::message::SIZE:
      return "SIZE";
    case query ::message::SEEN:
      return "SEEN";
    case query ::message::GPIC:
      return "GPIC";
    case query ::message::SPIC:
      return "SPIC";
    case query ::message::CHLD:
      return "CHLD";
  }
  abort();
}

const char *answer_to_string(answer::message q)
{
  switch (q)
  {
    case answer::DONE:
      return "DONE";
    case answer::PASS:
      return "PASS";
    case answer::SIZE:
      return "SIZE";
    case answer::READ:
      return "READ";
    case answer::FORK:
      return "FORK";
    case answer::OPEN:
      return "OPEN";
    case answer::GPIC:
      return "GPIC";
  }
  abort();
}

const char *ask_to_string(enum ask q)
{
  switch (q)
  {
    case C_FLSH: return "FLSH";
  }
  abort();
}

Channel::Channel(): buf(new char[256]), buf_size(256), passed_fd(-1) {}

Channel::~Channel() { delete[] this->buf; }

void Channel::resize_buf()
{
  int old_size = this->buf_size;
  int new_size = old_size * 2;
  char *buf = new char[new_size];
  if (!buf) mabort();
  memcpy(buf, this->buf, old_size);
  memset(buf + old_size, 0, old_size);
  delete[] this->buf;
  this->buf = buf;
  this->buf_size = new_size;
}

int Channel::cgetc(int fd)
{
  if (this->input.pos == this->input.len)
    if (!this->refill_at_least(fd, 1))
      return 0;
  return this->input.buffer[this->input.pos++];
}

int Channel::read_zstr(int fd, int *pos)
{
  int c, p0 = *pos;
  do {
    if (*pos == this->buf_size) this->resize_buf();
    c = this->cgetc(fd);
    this->buf[*pos] = c;
    *pos += 1;
  } while (c != 0);
  return p0;
}

bool Channel::read_bytes(int fd, int pos, int size)
{
  while (this->buf_size < pos + size) this->resize_buf();

  int ipos = this->input.pos, ilen = this->input.len;
  if (ipos + size <= ilen)
  {
    memcpy(this->buf + pos, this->input.buffer + ipos, size);
    this->input.pos += size;
    return 1;
  }

  int isize = ilen - ipos;
  memcpy(this->buf + pos, this->input.buffer + ipos, isize);
  pos += isize;
  size -= isize;
  this->input.pos = this->input.len = 0;
  return this->read_all(fd, this->buf + pos, size);
}

void Channel::write_bytes(int fd, void *buf, int size)
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

bool Channel::try_read_u32(int fd, uint32_t *tag)
{
  if (!this->refill_at_least(fd, 4))
    return 0;
  memcpy(tag, this->input.buffer + this->input.pos, 4);
  this->input.pos += 4;
  return 1;
}

uint32_t Channel::read_u32(int fd)
{
  int avail = this->input.len - this->input.pos;

  if (!this->refill_at_least(fd, 4)) return 0;

  uint32_t tag;
  memcpy(&tag, this->input.buffer + this->input.pos, 4);
  this->input.pos += 4;

  return tag;
}

void Channel::write_u32(int fd, uint32_t u)
{
  this->write_bytes(fd, &u, 4);
}

float Channel::read_f32(int fd)
{
  if (!this->refill_at_least(fd, 4)) return 0;

  float f;
  memcpy(&f, this->input.buffer + this->input.pos, 4);
  this->input.pos += 4;

  return f;
}

void Channel::write_f32(int fd, float f)
{
  this->write_bytes(fd, &f, 4);
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

bool Channel::has_pending_query(int fd, int timeout)
{
  if (this->input.pos != this->input.len) return 1;

  struct pollfd pfd;
  int n;
  while(1)
  {
    pfd.fd = fd;
    pfd.events = POLLRDNORM;
    pfd.revents = 0;
    n = poll(&pfd, 1, timeout);
    if (!(n == -1 && errno == EINTR)) break;
  }

  if (n == -1) pabort();
  if (n == 0) return 0;
  return 1;
}

std::optional<query::message> Channel::peek(int fd)
{
  uint32_t result = this->read_u32(fd);
  if (result == 0) abort();
  this->input.pos -= 4;
  switch (result) {
    case query::message::CHLD:
    case query::message::CLOS:
    case query::message::GPIC:
    case query::message::OPEN:
    case query::message::READ:
    case query::message::SEEN:
    case query::message::SIZE:
    case query::message::SPIC:
    case query::message::WRIT:
      return static_cast<query::message>(result);
      break;
    default:
      return {};
  }
}

std::optional<query::data> Channel::read_query(int fd)
{
  uint32_t tag;

  if (!this->try_read_u32(fd, &tag)) return {};
  int time = this->read_u32(fd);
  int pos = 0;
  switch (tag)
  {
    case query::OPEN:
    {
        int pos_path = this->read_zstr(fd, &pos);
        int pos_mode = this->read_zstr(fd, &pos);
        query::open op = {
            .fid = static_cast<file_id>(this->read_u32(fd)),
            .path = this->buf + pos_path,
            .mode = this->buf + pos_mode,
        };
        return query::data(time, op);
    }
    case query::READ:
    {
        return query::data(time, query::read {
            .fid = static_cast<file_id>(this->read_u32(fd)),
            .pos = static_cast<file_id>(this->read_u32(fd)),
            .size = static_cast<file_id>(this->read_u32(fd)),
        });
    }
    case query::WRIT:
    {
        query::writ wr {
            .fid = static_cast<file_id>(this->read_u32(fd)),
            .pos = static_cast<file_id>(this->read_u32(fd)),
            .size = static_cast<file_id>(this->read_u32(fd)),
        };
        if (!this->read_bytes(fd, 0, wr.size)) return {};
        wr.buf = this->buf;
        return query::data(time, wr);
    }
    case query::CLOS:
    {
        query::clos cl {
            .fid = static_cast<file_id>(this->read_u32(fd))
        };
        return query::data(time, cl);
    }
    case query::SIZE:
    {
        query::size si {
            .fid = static_cast<file_id>(this->read_u32(fd))
        };
        return query::data(time, si);
    }
    case query::SEEN:
    {
        query::seen se {
            .fid = static_cast<file_id>(this->read_u32(fd)),
            .pos = static_cast<file_id>(this->read_u32(fd)),
        };
        return query::data(time, se);
    }
    case query::GPIC:
    {
        int pos_path = this->read_zstr(fd, &pos);
        query::gpic gp {
            .path = this->buf + pos_path,
            .type = static_cast<file_id>(this->read_u32(fd)),
            .page = static_cast<file_id>(this->read_u32(fd)),
        };
        return query::data(time, gp);
    }
    case query::SPIC:
    {
        int pos_path = this->read_zstr(fd, &pos);
        query::spic sp {
            .path = this->buf + pos_path,
            .cache = {
                .type = static_cast<file_id>(this->read_u32(fd)),
                .page = static_cast<file_id>(this->read_u32(fd)),
                .bounds = {
                    this->read_f32(fd),
                    this->read_f32(fd),
                    this->read_f32(fd),
                    this->read_f32(fd),
                }
            }
        };
        return query::data(time, sp);
    }
    case query::CHLD:
    {
        query::chld ch {
            .pid = static_cast<file_id>(this->read_u32(fd)),
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
    }
  }
  // put log on the caller?
  // if (LOG)
  // {
  //   fprintf(stderr, "[info] <- ");
  //   log_query(stderr, r);
  // }
}

void Channel::write_ask(int fd, ask_t *a)
{
  this->write_u32(fd, a->tag);
  switch (a->tag)
  {
    case C_FLSH: break;
    default: mabort();
  }
}

void Channel::write_time(int fd, struct status::time tm)
{
  this->write_u32(fd, tm.sec);
  this->write_u32(fd, tm.nsec);
}

void Channel::write_answer(int fd, answer::data &a)
{
  // if (LOG)
  // {
  //   if (a->tag == READ)
  //     fprintf(stderr, "[info] -> READ %d\n", a->read.size);
  //   else
  //     fprintf(stderr, "[info] -> %s\n", answer_to_string(a->tag));
  // }
  this->write_u32(fd, static_cast<uint32_t>(a.to_enum()));
  std::visit(overloaded {
      [](answer::done _) { return; },
      [](answer::pass _) { return; },
      [](answer::fork _) { return; },
      [fd, this](answer::read r) {
          this->write_u32(fd, r.size);
          this->write_bytes(fd, this->buf, r.size);
      },
      [fd, this](answer::size s) {
          this->write_u32(fd, s.size);
      },
      [fd, this](answer::open o) {
          this->write_u32(fd, o.size);
          this->write_bytes(fd, this->buf, o.size);
      },
      [fd, this](answer::gpic g) {
          this->write_f32(fd, g.bounds[0]);
          this->write_f32(fd, g.bounds[1]);
          this->write_f32(fd, g.bounds[2]);
          this->write_f32(fd, g.bounds[3]);
      },
  }, a);
}

void Channel::flush(int fd)
{
  this->cflush(fd);
}

void Channel::reset()
{
  this->input.pos = this->input.len = 0;
  this->output.pos = 0;
}

void *Channel::get_buffer(size_t n)
{
  while (n > this->buf_size) this->resize_buf();
  return this->buf;
}
