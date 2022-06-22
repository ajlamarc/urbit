//! @file cttp.c

#include "vere/vere.h"

//==============================================================================
// Types
//==============================================================================

typedef struct {
  u3_auto        driver_u;   //!< driver handle
  c3_l           inst_num_l; //!< instance number
  struct {
    uv_process_t proc_u;     //!< process handle
    uv_pipe_t    stdin_u;    //!< stdin stream to IO process
    uv_pipe_t    stdout_u;   //!< stdout stream to IO process
  } child_u;                 //!< IO process
} _client;

typedef struct {
  c3_d  len_d;        //!< combined length of type and jammed request
  c3_y  type_y;       //!< type of IO request (0 = HTTP client)
  c3_y* jammed_req_y; //!< jammed request
} _io_req;

//==============================================================================
// Constants
//==============================================================================

//! Request types for IPC pipe. Belongs in header file shared by all IO drivers
//! long term.
enum {
  IO_REQ_HTTP_CLIENT = 0,
};

//==============================================================================
// Static functions
//==============================================================================

static void
_child_exit_cb(uv_process_t* child_u, c3_ds status_d, c3_i term_sig_i);

static void
_driver_exit(u3_auto* driver_u);

static c3_o
_driver_kick(u3_auto* driver_u, u3_noun wire, u3_noun card);

static void
_driver_talk(u3_auto* driver_u);

static void
_write_cb(uv_write_t* req_u, c3_i status_i);

static void
_child_exit_cb(uv_process_t* child_u, c3_ds status_d, c3_i term_sig_i)
{}

static void
_driver_exit(u3_auto* driver_u)
{
  _client* client_u = (_client*)driver_u;
  c3_free(client_u);
}

// card is [tag data].
static c3_o
_driver_kick(u3_auto* driver_u, u3_noun wire, u3_noun card)
{
  c3_o suc_o = c3n;

  u3_noun tag, data, wire_head;
  if ( (c3n == u3r_cell(wire, &wire_head, NULL))
       || (c3n == u3r_sing_c("http-client", wire_head)) )
  {
    goto end;
  }

  _client* client_u = (_client*)driver_u;
  _io_req* io_req_u = c3_malloc(sizeof(*io_req_u));
  u3s_jam_xeno(card, &io_req_u->len_d, &io_req_u->jammed_req_y);
  io_req_u->type_y = IO_REQ_HTTP_CLIENT;
  io_req_u->len_d += sizeof(io_req_u->type_y);

  uv_buf_t req_bufs_u[] = {
    {
      // Request length.
      .base = (c3_c*)&io_req_u->len_d,
      .len = sizeof(io_req_u->len_d),
    },
    {
      // Request type.
      .base = (c3_c*)&io_req_u->type_y,
      .len = sizeof(io_req_u->type_y),
    },
    {
      // Jammed request.
      .base = (c3_c*)io_req_u->jammed_req_y,
      .len = io_req_u->len_d - sizeof(io_req_u->type_y),
    },
  };

  uv_write_t* write_req_u = c3_malloc(sizeof(*write_req_u));
  uv_write(write_req_u,
           (uv_stream_t*)&client_u->child_u.stdin_u,
           req_bufs_u,
           sizeof(req_bufs_u) / sizeof(*req_bufs_u),
           _write_cb);

end:
  u3z(wire);
  u3z(card);
  return suc_o;
}

//! Notify that the HTTP client driver is live.
static void
_driver_talk(u3_auto* driver_u)
{
  _client* client_u = (_client*)driver_u;

  u3_noun wire = u3nt(u3i_string("http-client"),
                      u3dc("scot", c3__uv, client_u->inst_num_l),
                      u3_nul);
  u3_noun card = u3nc(c3__born, u3_nul);

  u3_auto_plan(driver_u, u3_ovum_init(0, c3__i, wire, card));
}

static void
_write_cb(uv_write_t* write_req_u, c3_i status_i)
{
  c3_free(write_req_u);
}

//==============================================================================
// Functions
//==============================================================================

u3_auto*
u3_cttp_io_init(u3_pier* pir_u)
{
  _client* client_u = c3_calloc(sizeof(*client_u));

  client_u->driver_u = (u3_auto){
    .nam_m = c3__cttp,
    .liv_o = c3y,
    .io.talk_f = _driver_talk,
    .io.kick_f = _driver_kick,
    .io.exit_f = _driver_exit,
  };

  {
    struct timeval time_u;
    gettimeofday(&time_u, NULL);
    u3_noun now          = u3_time_in_tv(&time_u);
    client_u->inst_num_l = u3r_mug(now);
    u3z(now);
  }

  {
    // TODO: integrate the Rust binary.
    c3_c* args_c[] = {
      "/home/tlon/code/io/target/debug/io",
      NULL,
    };

    uv_pipe_init(u3L, &client_u->child_u.stdin_u, 0);
    //uv_pipe_init(u3L, &client_u->child_u.stdout_u, 0);
    uv_stdio_container_t stdio_u[] = {
      {
        // stdin pipe
        .flags       = UV_CREATE_PIPE | UV_READABLE_PIPE,
        .data.stream = (uv_stream_t*)&client_u->child_u.stdin_u,
      },
      {
        // stdout pipe
        //.flags       = UV_CREATE_PIPE | UV_WRITABLE_PIPE,
        //.data.stream = (uv_stream_t*)&client_u->child_u.stdout_u,
        .flags       = UV_INHERIT_FD,
        .data.fd     = STDOUT_FILENO,
      },
      {
        // stderr pipe
        .flags       = UV_INHERIT_FD,
        .data.fd     = STDERR_FILENO,
      },
    };

    uv_process_options_t opt_u = {
      .exit_cb     = _child_exit_cb,
      .file        = args_c[0],
      .args        = args_c,
      // If any fds are inherited, libuv ignores UV_PROCESS_WINDOWS_HIDE*.
      .flags       = UV_PROCESS_WINDOWS_HIDE,
      .stdio_count = sizeof(stdio_u) / sizeof(*stdio_u),
      .stdio       = stdio_u,
    };

    c3_i ret_i = uv_spawn(u3L, &client_u->child_u.proc_u, &opt_u);
    if ( 0 != ret_i ) {
      fprintf(stderr,
              "http-client: failed to spawn %s: %s\r\n",
              args_c[0],
              uv_strerror(ret_i));
      // TODO: do the pipes need to be torn down?
      c3_free(client_u);
      return NULL;
    }
  }

  return (u3_auto*)client_u;
}
