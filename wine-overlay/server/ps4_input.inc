/* WinePS4: the desktop's input (port/include/ps4_display.h), included at the end of queue.c.
 *
 * The app's wineserver runs as a thread of the desktop process: the desktop writes the PS4's mouse,
 * pad and keyboard into a pipe as records, and the server reads them in its own loop and queues
 * them like a PC's hardware input (as send_hardware_message does), so the window under the cursor or
 * with the focus gets them. No thread of the server is "current" here: hardware input from the
 * system has no sender. */

#include <fcntl.h>
#include <string.h>
#include "ps4_display.h"

struct ps4_input
{
    struct object obj;
    struct fd    *fd;
};

static void ps4_input_dump( struct object *obj, int verbose )
{
    fprintf( stderr, "WinePS4 input pipe\n" );
}

static void ps4_input_destroy( struct object *obj )
{
    struct ps4_input *input = (struct ps4_input *)obj;
    release_object( input->fd );
}

static const struct object_ops ps4_input_ops =
{
    sizeof(struct ps4_input),      /* size */
    &no_type,                      /* type */
    ps4_input_dump,                /* dump */
    no_add_queue,                  /* add_queue */
    NULL,                          /* remove_queue */
    NULL,                          /* signaled */
    NULL,                          /* satisfied */
    no_signal,                     /* signal */
    no_get_fd,                     /* get_fd */
    default_get_sync,              /* get_sync */
    default_map_access,            /* map_access */
    default_get_sd,                /* get_sd */
    default_set_sd,                /* set_sd */
    no_get_full_name,              /* get_full_name */
    no_lookup_name,                /* lookup_name */
    no_link_name,                  /* link_name */
    NULL,                          /* unlink_name */
    no_open_file,                  /* open_file */
    no_kernel_obj_list,            /* get_kernel_obj_list */
    no_close_handle,               /* close_handle */
    ps4_input_destroy              /* destroy */
};

static int ps4_input_pipe[2] = { -1, -1 };

extern void ps4_end_programs_now(void);

static void ps4_input_queue( const struct ps4_input_record *record )
{
    struct desktop *desktop;
    union hw_input input;

    if (record->type == PS4_INPUT_END_PROGRAMS)
    {
        ps4_end_programs_now();
        return;
    }

    /* none before the first program made the window station */
    if (!(desktop = get_hardware_input_desktop( 0 ))) return;
    if (!set_input_desktop( desktop->winstation, desktop ))
    {
        release_object( desktop );
        return;
    }
    memset( &input, 0, sizeof(input) );
    switch (record->type)
    {
    case PS4_INPUT_MOUSE:
        input.type        = INPUT_MOUSE;
        input.mouse.x     = record->x;
        input.mouse.y     = record->y;
        input.mouse.data  = record->data;
        input.mouse.flags = record->flags | MOUSEEVENTF_ABSOLUTE;
        queue_mouse_message( desktop, 0, &input, IMO_HARDWARE, NULL );
        break;
    case PS4_INPUT_KEY:
        input.type       = INPUT_KEYBOARD;
        input.kbd.vkey   = record->vkey;
        input.kbd.scan   = record->scan;
        input.kbd.flags  = record->flags;
        queue_keyboard_message( desktop, 0, &input, IMO_HARDWARE, NULL, 0 );
        break;
    }
    release_object( desktop );
}

static void ps4_input_poll_event( struct fd *fd, int event )
{
    struct ps4_input_record records[64];
    ssize_t got;
    int i;

    if (event & (POLLERR | POLLHUP))
    {
        set_fd_events( fd, -1 );
        return;
    }
    while ((got = read( ps4_input_pipe[0], records, sizeof(records) )) > 0)
        for (i = 0; i < got / (ssize_t)sizeof(records[0]); i++) ps4_input_queue( &records[i] );
}

static const struct fd_ops ps4_input_fd_ops =
{
    NULL,                          /* get_poll_events */
    ps4_input_poll_event,          /* poll_event */
    NULL,                          /* flush */
    NULL,                          /* get_fd_type */
    NULL,                          /* ioctl */
    NULL,                          /* queue_async */
    NULL                           /* reselect_async */
};

/* called by ps4_wineserver_main for the app's server */
void ps4_input_init(void)
{
    struct ps4_input *input;

    if (pipe( ps4_input_pipe )) return;
    fcntl( ps4_input_pipe[0], F_SETFL, O_NONBLOCK );
    if (!(input = alloc_object( &ps4_input_ops ))) return;
    if (!(input->fd = create_anonymous_fd( &ps4_input_fd_ops, ps4_input_pipe[0], &input->obj, 0 )))
    {
        release_object( input );
        return;
    }
    set_fd_events( input->fd, POLLIN );
    make_object_permanent( &input->obj );
}

/* the desktop's thread: records are small, a pipe write of one is atomic */
int ps4_server_input( const struct ps4_input_record *record )
{
    if (ps4_input_pipe[1] < 0) return 0;
    return write( ps4_input_pipe[1], record, sizeof(*record) ) == sizeof(*record);
}
