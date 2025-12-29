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

  GstPad *sinkpad;
  GstPad *srcpad;

  gchar *uri;
  guint sample_rate;
  guint channels;
  guint frame_duration_ms;
  guint max_queue_size;
  guint initial_buffer_count;

  SoupSession *session;
  SoupWebsocketConnection *ws_conn;
  GMainContext *context;
  GMainLoop *loop;
  GThread *ws_thread;

  guint bytes_per_sample;
  guint frame_size_bytes;
  GstClockTime frame_duration;

  GstClockTime base_timestamp;
  GstClockTime next_timestamp;
  gboolean first_timestamp_set;

  GQueue *recv_queue;
  GMutex queue_lock;

  GThread *output_thread;
  gboolean output_thread_running;
  gboolean ws_thread_running;
  GMutex output_lock;
  GCond output_cond;

  gboolean connected;
  gboolean eos_sent;
  GMutex state_lock;
  GCond connect_cond;

  GCond queue_cond;
  GCond caps_cond;
  gboolean caps_ready;
  gboolean reconnect_enabled;
  guint initial_reconnect_delay_ms;
  guint max_backoff_ms;
  guint max_reconnects;
  guint reconnect_count;
  guint current_backoff_ms;
};


struct _GstWebSocketTransceiverClass
{
  GstElementClass parent_class;
};

GType gst_websocket_transceiver_get_type(void);

G_END_DECLS

#endif /* __GST_WEBSOCKET_TRANSCEIVER_H__ */
