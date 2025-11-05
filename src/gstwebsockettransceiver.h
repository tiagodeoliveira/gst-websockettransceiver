#ifndef __GST_WEBSOCKET_TRANSCEIVER_H__
#define __GST_WEBSOCKET_TRANSCEIVER_H__

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

#define GST_TYPE_WEBSOCKET_TRANSCEIVER \
  (gst_websocket_transceiver_get_type())
#define GST_WEBSOCKET_TRANSCEIVER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_WEBSOCKET_TRANSCEIVER, GstWebSocketTransceiver))
#define GST_WEBSOCKET_TRANSCEIVER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_WEBSOCKET_TRANSCEIVER, GstWebSocketTransceiverClass))
#define GST_IS_WEBSOCKET_TRANSCEIVER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_WEBSOCKET_TRANSCEIVER))
#define GST_IS_WEBSOCKET_TRANSCEIVER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_WEBSOCKET_TRANSCEIVER))

typedef struct _GstWebSocketTransceiver GstWebSocketTransceiver;
typedef struct _GstWebSocketTransceiverClass GstWebSocketTransceiverClass;


struct _GstWebSocketTransceiver
{
  GstElement parent;

  /* Pads */
  GstPad *sinkpad;
  GstPad *srcpad;

  /* Properties */
  gchar *uri;
  guint sample_rate;
  guint channels;
  guint frame_duration_ms;  /* Frame duration in milliseconds */
  guint max_queue_size;     /* Maximum receive queue size in buffers */
  guint initial_buffer_count; /* Number of buffers to accumulate before starting playback */

  /* WebSocket */
  SoupSession *session;
  SoupWebsocketConnection *ws_conn;
  GMainContext *context;
  GMainLoop *loop;
  GThread *ws_thread;

  /* Audio parameters */
  guint bytes_per_sample;
  guint frame_size_bytes; 
  GstClockTime frame_duration; 

  /* Timing */
  GstClockTime base_timestamp;
  GstClockTime next_timestamp;
  gboolean first_timestamp_set;

  /* Data queue for received audio */
  GQueue *recv_queue;
  GMutex queue_lock;

  /* Output thread */
  GThread *output_thread;
  gboolean output_thread_running;
  GMutex output_lock;
  GCond output_cond;

  /* State */
  gboolean connected;
  gboolean eos_sent;  /* Flag to prevent duplicate EOS events */
  GMutex state_lock;
};

struct _GstWebSocketTransceiverClass
{
  GstElementClass parent_class;
};

GType gst_websocket_transceiver_get_type(void);

G_END_DECLS

#endif /* __GST_WEBSOCKET_TRANSCEIVER_H__ */
