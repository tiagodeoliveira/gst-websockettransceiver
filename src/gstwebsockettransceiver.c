#include "gstwebsockettransceiver.h"
#include <string.h>
#include <gst/audio/audio.h>
#include <json-glib/json-glib.h>

GST_DEBUG_CATEGORY_STATIC(gst_websocket_transceiver_debug);
#define GST_CAT_DEFAULT gst_websocket_transceiver_debug

enum
{
  PROP_0,
  PROP_URI,
  PROP_SAMPLE_RATE,
  PROP_CHANNELS,
  PROP_FRAME_DURATION_MS,
  PROP_MAX_QUEUE_SIZE,
  PROP_INITIAL_BUFFER_COUNT,
  PROP_RECONNECT_ENABLED,
  PROP_INITIAL_RECONNECT_DELAY_MS,
  PROP_MAX_BACKOFF_MS,
  PROP_MAX_RECONNECTS,
};

#define DEFAULT_URI NULL
#define DEFAULT_SAMPLE_RATE 16000
#define DEFAULT_CHANNELS 1
#define DEFAULT_FRAME_DURATION_MS 250
#define DEFAULT_MAX_QUEUE_SIZE 100
#define DEFAULT_INITIAL_BUFFER_COUNT 3

#define DEFAULT_RECONNECT_ENABLED TRUE
#define DEFAULT_INITIAL_RECONNECT_DELAY_MS 1000
#define DEFAULT_MAX_BACKOFF_MS 30000
#define DEFAULT_MAX_RECONNECTS 10

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, "
        "format = (string) { S16LE, S16BE, S32LE, S32BE, F32LE, F32BE }, "
        "rate = (int) [ 8000, 48000 ], "
        "channels = (int) [ 1, 2 ], "
        "layout = (string) interleaved; "
        "audio/x-mulaw, "
        "rate = (int) [ 8000, 48000 ], "
        "channels = (int) [ 1, 2 ]; "
        "audio/x-alaw, "
        "rate = (int) [ 8000, 48000 ], "
        "channels = (int) [ 1, 2 ]"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, "
        "format = (string) { S16LE, S16BE, S32LE, S32BE, F32LE, F32BE }, "
        "rate = (int) [ 8000, 48000 ], "
        "channels = (int) [ 1, 2 ], "
        "layout = (string) interleaved; "
        "audio/x-mulaw, "
        "rate = (int) [ 8000, 48000 ], "
        "channels = (int) [ 1, 2 ]; "
        "audio/x-alaw, "
        "rate = (int) [ 8000, 48000 ], "
        "channels = (int) [ 1, 2 ]"));

#define gst_websocket_transceiver_parent_class parent_class
G_DEFINE_TYPE(GstWebSocketTransceiver, gst_websocket_transceiver, GST_TYPE_ELEMENT);

static void gst_websocket_transceiver_finalize(GObject *object);
static void gst_websocket_transceiver_set_property(GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec);
static void gst_websocket_transceiver_get_property(GObject *object, guint prop_id,
    GValue *value, GParamSpec *pspec);
static GstStateChangeReturn gst_websocket_transceiver_change_state(GstElement *element,
    GstStateChange transition);
static GstFlowReturn gst_websocket_transceiver_chain(GstPad *pad, GstObject *parent,
    GstBuffer *buffer);
static gboolean gst_websocket_transceiver_src_query(GstPad *pad, GstObject *parent,
    GstQuery *query);
static gboolean gst_websocket_transceiver_sink_event(GstPad *pad, GstObject *parent,
    GstEvent *event);
static gboolean gst_websocket_transceiver_sink_setcaps(GstWebSocketTransceiver *self,
    GstCaps *caps);
static gpointer gst_websocket_transceiver_output_thread(gpointer user_data);
static gpointer gst_websocket_transceiver_ws_thread(gpointer user_data);

static void
gst_websocket_transceiver_class_init(GstWebSocketTransceiverClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS(klass);

  gobject_class->set_property = gst_websocket_transceiver_set_property;
  gobject_class->get_property = gst_websocket_transceiver_get_property;
  gobject_class->finalize = gst_websocket_transceiver_finalize;

  g_object_class_install_property(gobject_class, PROP_URI,
      g_param_spec_string("uri", "URI", "WebSocket URI to connect to",
          DEFAULT_URI, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_SAMPLE_RATE,
      g_param_spec_uint("sample-rate", "Sample Rate", "Audio sample rate",
          1, G_MAXUINT, DEFAULT_SAMPLE_RATE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_CHANNELS,
      g_param_spec_uint("channels", "Channels", "Number of audio channels",
          1, 2, DEFAULT_CHANNELS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_FRAME_DURATION_MS,
      g_param_spec_uint("frame-duration-ms", "Frame Duration",
          "Frame duration in milliseconds",
          10, 1000, DEFAULT_FRAME_DURATION_MS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_MAX_QUEUE_SIZE,
      g_param_spec_uint("max-queue-size", "Max Queue Size",
          "Maximum receive queue size in buffers",
          1, 1000, DEFAULT_MAX_QUEUE_SIZE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_INITIAL_BUFFER_COUNT,
      g_param_spec_uint("initial-buffer-count", "Initial Buffer Count",
          "Number of buffers to accumulate before starting playback (0 = no buffering)",
          0, 100, DEFAULT_INITIAL_BUFFER_COUNT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_RECONNECT_ENABLED,
      g_param_spec_boolean("reconnect-enabled", "Reconnect Enabled",
          "Enable automatic WebSocket reconnection on disconnect/error",
          DEFAULT_RECONNECT_ENABLED, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_INITIAL_RECONNECT_DELAY_MS,
      g_param_spec_uint("initial-reconnect-delay-ms", "Initial Reconnect Delay",
          "Initial reconnection delay in ms (exponential backoff starts here)",
          100, 5000, DEFAULT_INITIAL_RECONNECT_DELAY_MS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_MAX_BACKOFF_MS,
      g_param_spec_uint("max-backoff-ms", "Max Backoff Delay",
          "Maximum backoff delay in ms",
          1000, 60000, DEFAULT_MAX_BACKOFF_MS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_MAX_RECONNECTS,
      g_param_spec_uint("max-reconnects", "Max Reconnects",
          "Maximum reconnection attempts (0 = infinite if reconnect-enabled)",
          0, 100, DEFAULT_MAX_RECONNECTS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstelement_class->change_state = gst_websocket_transceiver_change_state;

  gst_element_class_set_static_metadata(gstelement_class,
      "WebSocket Audio Transceiver",
      "Source/Sink/Network",
      "Sends and receives audio over WebSocket for AI voice bots",
      "Tiago de Oliveira <tiagode@amazon.com>");

  gst_element_class_add_static_pad_template(gstelement_class, &src_template);
  gst_element_class_add_static_pad_template(gstelement_class, &sink_template);

  GST_DEBUG_CATEGORY_INIT(gst_websocket_transceiver_debug, "websockettransceiver",
      0, "WebSocket Audio Transceiver");
}

static void
gst_websocket_transceiver_init(GstWebSocketTransceiver *self)
{
  self->sinkpad = gst_pad_new_from_static_template(&sink_template, "sink");
  gst_pad_set_chain_function(self->sinkpad, gst_websocket_transceiver_chain);
  gst_pad_set_event_function(self->sinkpad, gst_websocket_transceiver_sink_event);
  gst_element_add_pad(GST_ELEMENT(self), self->sinkpad);

  self->srcpad = gst_pad_new_from_static_template(&src_template, "src");
  gst_pad_set_query_function(self->srcpad, gst_websocket_transceiver_src_query);
  gst_element_add_pad(GST_ELEMENT(self), self->srcpad);

  self->uri = NULL;
  self->sample_rate = DEFAULT_SAMPLE_RATE;
  self->channels = DEFAULT_CHANNELS;
  self->frame_duration_ms = DEFAULT_FRAME_DURATION_MS;
  self->max_queue_size = DEFAULT_MAX_QUEUE_SIZE;
  self->initial_buffer_count = DEFAULT_INITIAL_BUFFER_COUNT;
  self->reconnect_enabled = DEFAULT_RECONNECT_ENABLED;
  self->initial_reconnect_delay_ms = DEFAULT_INITIAL_RECONNECT_DELAY_MS;
  self->max_backoff_ms = DEFAULT_MAX_BACKOFF_MS;
  self->max_reconnects = DEFAULT_MAX_RECONNECTS;
  self->reconnect_count = 0;
  self->current_backoff_ms = 0;

  self->bytes_per_sample = 0;
  self->frame_size_bytes = 0;
  self->frame_duration = self->frame_duration_ms * GST_MSECOND;

  self->base_timestamp = GST_CLOCK_TIME_NONE;
  self->next_timestamp = 0;
  self->first_timestamp_set = FALSE;
  self->need_segment = FALSE;

  self->recv_queue = g_queue_new();
  g_mutex_init(&self->queue_lock);

  g_mutex_init(&self->output_lock);
  g_cond_init(&self->output_cond);
  self->output_thread_running = FALSE;
  self->ws_thread_running = FALSE;

  self->connected = FALSE;
  self->eos_sent = FALSE;
  g_mutex_init(&self->state_lock);
  g_cond_init(&self->connect_cond);

  g_cond_init(&self->queue_cond);
  g_cond_init(&self->caps_cond);
  self->caps_ready = FALSE;

  self->session = NULL;
  self->ws_conn = NULL;
  self->context = NULL;
  self->loop = NULL;
  self->ws_thread = NULL;
  self->output_thread = NULL;

  // mark as live source for real-time data production
  GST_OBJECT_FLAG_SET(self, GST_ELEMENT_FLAG_SOURCE);
}

static void
gst_websocket_transceiver_finalize(GObject *object)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(object);

  g_free(self->uri);

  g_mutex_lock(&self->queue_lock);
  g_queue_free_full(self->recv_queue, (GDestroyNotify)gst_buffer_unref);
  g_mutex_unlock(&self->queue_lock);

  g_mutex_clear(&self->queue_lock);
  g_mutex_clear(&self->output_lock);
  g_cond_clear(&self->output_cond);
  g_mutex_clear(&self->state_lock);
  g_cond_clear(&self->connect_cond);
  g_cond_clear(&self->queue_cond);
  g_cond_clear(&self->caps_cond);

  G_OBJECT_CLASS(parent_class)->finalize(object);
}

static void
gst_websocket_transceiver_set_property(GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(object);

  switch (prop_id) {
    case PROP_URI:
      g_free(self->uri);
      self->uri = g_value_dup_string(value);
      break;
    case PROP_SAMPLE_RATE:
      self->sample_rate = g_value_get_uint(value);
      self->frame_size_bytes = (self->sample_rate * self->bytes_per_sample *
                                self->channels * self->frame_duration_ms) / 1000;
      break;
    case PROP_CHANNELS:
      self->channels = g_value_get_uint(value);
      self->frame_size_bytes = (self->sample_rate * self->bytes_per_sample *
                                self->channels * self->frame_duration_ms) / 1000;
      break;
    case PROP_FRAME_DURATION_MS:
      self->frame_duration_ms = g_value_get_uint(value);
      self->frame_size_bytes = (self->sample_rate * self->bytes_per_sample *
                                self->channels * self->frame_duration_ms) / 1000;
      self->frame_duration = self->frame_duration_ms * GST_MSECOND;
      break;
    case PROP_MAX_QUEUE_SIZE:
      self->max_queue_size = g_value_get_uint(value);
      break;
    case PROP_INITIAL_BUFFER_COUNT:
      self->initial_buffer_count = g_value_get_uint(value);
      break;
    case PROP_RECONNECT_ENABLED:
      self->reconnect_enabled = g_value_get_boolean(value);
      break;
    case PROP_INITIAL_RECONNECT_DELAY_MS:
      self->initial_reconnect_delay_ms = g_value_get_uint(value);
      break;
    case PROP_MAX_BACKOFF_MS:
      self->max_backoff_ms = g_value_get_uint(value);
      break;
    case PROP_MAX_RECONNECTS:
      self->max_reconnects = g_value_get_uint(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void
gst_websocket_transceiver_get_property(GObject *object, guint prop_id,
    GValue *value, GParamSpec *pspec)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(object);

  switch (prop_id) {
    case PROP_URI:
      g_value_set_string(value, self->uri);
      break;
    case PROP_SAMPLE_RATE:
      g_value_set_uint(value, self->sample_rate);
      break;
    case PROP_CHANNELS:
      g_value_set_uint(value, self->channels);
      break;
    case PROP_FRAME_DURATION_MS:
      g_value_set_uint(value, self->frame_duration_ms);
      break;
    case PROP_MAX_QUEUE_SIZE:
      g_value_set_uint(value, self->max_queue_size);
      break;
    case PROP_INITIAL_BUFFER_COUNT:
      g_value_set_uint(value, self->initial_buffer_count);
      break;
    case PROP_RECONNECT_ENABLED:
      g_value_set_boolean(value, self->reconnect_enabled);
      break;
    case PROP_INITIAL_RECONNECT_DELAY_MS:
      g_value_set_uint(value, self->initial_reconnect_delay_ms);
      break;
    case PROP_MAX_BACKOFF_MS:
      g_value_set_uint(value, self->max_backoff_ms);
      break;
    case PROP_MAX_RECONNECTS:
      g_value_set_uint(value, self->max_reconnects);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_websocket_transceiver_src_query(GstPad *pad, GstObject *parent, GstQuery *query)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(parent);
  gboolean ret = TRUE;

  switch (GST_QUERY_TYPE(query)) {
    case GST_QUERY_LATENCY:
    {
      GstClockTime min_latency = self->frame_duration;
      GstClockTime max_latency = self->frame_duration * self->max_queue_size;

      gst_query_set_latency(query, TRUE, min_latency, max_latency);
      GST_DEBUG_OBJECT(self, "Reporting latency: min=%" GST_TIME_FORMAT
          " max=%" GST_TIME_FORMAT,
          GST_TIME_ARGS(min_latency), GST_TIME_ARGS(max_latency));
      break;
    }
    case GST_QUERY_SCHEDULING:
    {
      gst_query_set_scheduling(query, GST_SCHEDULING_FLAG_SEQUENTIAL, 1, -1, 0);
      gst_query_add_scheduling_mode(query, GST_PAD_MODE_PUSH);
      break;
    }
    default:
      ret = gst_pad_query_default(pad, parent, query);
      break;
  }

  return ret;
}

static gboolean
gst_websocket_transceiver_sink_setcaps(GstWebSocketTransceiver *self, GstCaps *caps)
{
  GstStructure *structure;
  const gchar *format_name;
  gint rate = 0, channels = 0;

  structure = gst_caps_get_structure(caps, 0);
  format_name = gst_structure_get_name(structure);

  if (!gst_structure_get_int(structure, "rate", &rate) ||
      !gst_structure_get_int(structure, "channels", &channels)) {
    GST_ERROR_OBJECT(self, "Caps missing rate or channels: %" GST_PTR_FORMAT, caps);
    return FALSE;
  }

  self->sample_rate = rate;
  self->channels = channels;

  if (g_str_equal(format_name, "audio/x-raw")) {
    GstAudioInfo info;
    if (gst_audio_info_from_caps(&info, caps)) {
      self->bytes_per_sample = GST_AUDIO_INFO_BPF(&info) / self->channels;
      GST_INFO_OBJECT(self, "Raw audio format: %s, %d bytes/sample",
          gst_audio_format_to_string(GST_AUDIO_INFO_FORMAT(&info)),
          self->bytes_per_sample);
    } else {
      GST_WARNING_OBJECT(self, "Failed to parse audio/x-raw caps, assuming 2 bytes/sample");
      self->bytes_per_sample = 2;
    }
  } else if (g_str_equal(format_name, "audio/x-mulaw") ||
             g_str_equal(format_name, "audio/x-alaw")) {
    self->bytes_per_sample = 1;
    GST_INFO_OBJECT(self, "Compressed audio format: %s, 1 byte/sample", format_name);
  } else {
    GST_WARNING_OBJECT(self, "Unknown audio format %s, assuming 1 byte/sample", format_name);
    self->bytes_per_sample = 1;
  }

  self->frame_size_bytes = (self->sample_rate * self->bytes_per_sample *
                            self->channels * self->frame_duration_ms) / 1000;
  self->frame_duration = self->frame_duration_ms * GST_MSECOND;

  GST_INFO_OBJECT(self, "Caps negotiated: format=%s, rate=%d Hz, channels=%d, "
      "bytes_per_sample=%d, frame_size=%d bytes (%.1f ms)",
      format_name, self->sample_rate, self->channels,
      self->bytes_per_sample, self->frame_size_bytes,
      (float)self->frame_duration_ms);

  if (!gst_pad_set_caps(self->srcpad, caps)) {
    GST_ERROR_OBJECT(self, "Failed to set caps on src pad");
    return FALSE;
  }

  g_mutex_lock(&self->state_lock);
  self->caps_ready = TRUE;
  g_cond_signal(&self->caps_cond);
  g_mutex_unlock(&self->state_lock);

  return TRUE;
}

static gboolean
gst_websocket_transceiver_sink_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(parent);
  gboolean ret = TRUE;

  switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;
      gst_event_parse_caps(event, &caps);
      ret = gst_websocket_transceiver_sink_setcaps(self, caps);
      gst_event_unref(event);
      break;
    }
    case GST_EVENT_EOS:
    {
      // eos from sink is ignored, we send eos on src pad when websocket closes
      GST_INFO_OBJECT(self, "Received EOS on sink pad (input stream ended) - ignoring, will send EOS when WebSocket closes");
      gst_event_unref(event);
      ret = TRUE;
      break;
    }
    default:
      ret = gst_pad_event_default(pad, parent, event);
      break;
  }

  return ret;
}

static void
gst_websocket_transceiver_flush_queue(GstWebSocketTransceiver *self)
{
  GST_INFO_OBJECT(self, "Flushing receive queue (barge-in)");

  g_mutex_lock(&self->queue_lock);
  while (!g_queue_is_empty(self->recv_queue)) {
    GstBuffer *buf = g_queue_pop_head(self->recv_queue);
    gst_buffer_unref(buf);
  }
  g_mutex_unlock(&self->queue_lock);

  g_mutex_lock(&self->output_lock);
  self->next_timestamp = 0;
  self->first_timestamp_set = FALSE;
  g_mutex_unlock(&self->output_lock);

  gst_pad_push_event(self->srcpad, gst_event_new_flush_start());
  gst_pad_push_event(self->srcpad, gst_event_new_flush_stop(TRUE));

  g_mutex_lock(&self->output_lock);
  self->need_segment = TRUE;
  g_mutex_unlock(&self->output_lock);

  GST_DEBUG_OBJECT(self, "Queue flushed, timestamps reset");
}

static void
on_websocket_message(SoupWebsocketConnection *conn, gint type, GBytes *message,
    gpointer user_data)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(user_data);
  GstBuffer *buffer;
  gconstpointer data;
  gsize size;

  data = g_bytes_get_data(message, &size);

  if (type == SOUP_WEBSOCKET_DATA_TEXT) {
    GST_DEBUG_OBJECT(self, "Received text message: %.*s", (int)size, (const gchar *)data);

    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    gboolean handled = FALSE;

    if (json_parser_load_from_data(parser, data, size, &error)) {
      JsonNode *root = json_parser_get_root(parser);
      if (root && JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject *obj = json_node_get_object(root);
        const gchar *msg_type = json_object_get_string_member_with_default(obj, "type", NULL);
        if (msg_type && g_strcmp0(msg_type, "clear") == 0) {
          gst_websocket_transceiver_flush_queue(self);
          handled = TRUE;
        }
      }
    } else {
      GST_WARNING_OBJECT(self, "Failed to parse JSON: %s", error->message);
      g_error_free(error);
    }

    if (!handled) {
      GST_WARNING_OBJECT(self, "Unknown control message: %.*s", (int)size, (const gchar *)data);
    }

    g_object_unref(parser);
    return;
  }

  if (type != SOUP_WEBSOCKET_DATA_BINARY) {
    GST_WARNING_OBJECT(self, "Received unknown WebSocket message type: %d", type);
    return;
  }

  GST_DEBUG_OBJECT(self, "Received WebSocket message: %zu bytes", size);

  buffer = gst_buffer_new_allocate(NULL, size, NULL);
  gst_buffer_fill(buffer, 0, data, size);

  g_mutex_lock(&self->queue_lock);

  while (g_queue_get_length(self->recv_queue) >= self->max_queue_size) {
    GstBuffer *dropped = g_queue_pop_head(self->recv_queue);
    gst_buffer_unref(dropped);
    GST_WARNING_OBJECT(self, "Queue full (%u), dropped old buffer", self->max_queue_size);
  }

  g_queue_push_tail(self->recv_queue, buffer);
  GST_DEBUG_OBJECT(self, "Queued buffer, queue length: %u",
      g_queue_get_length(self->recv_queue));

  g_cond_signal(&self->queue_cond);
  g_mutex_unlock(&self->queue_lock);
}

static void
on_websocket_error(SoupWebsocketConnection *conn, GError *error, gpointer user_data)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(user_data);
  GST_ERROR_OBJECT(self, "WebSocket error: %s", error ? error->message : "unknown");
  if (self->loop)
    g_main_loop_quit(self->loop);
}

static void
on_websocket_closed(SoupWebsocketConnection *conn, gpointer user_data)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(user_data);
  guint close_code;
  const gchar *close_data;

  close_code = soup_websocket_connection_get_close_code(conn);
  close_data = soup_websocket_connection_get_close_data(conn);

  GST_WARNING_OBJECT(self, "WebSocket connection closed (code: %u, reason: %s)",
      close_code, close_data ? close_data : "none");

  // mark as disconnected, output thread will drain queue before sending eos
  g_mutex_lock(&self->state_lock);
  self->connected = FALSE;
  g_mutex_unlock(&self->state_lock);

  GST_INFO_OBJECT(self, "WebSocket disconnected, output thread will drain queue and send EOS");
  if (self->loop)
    g_main_loop_quit(self->loop);
}

static void
on_websocket_connected(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(user_data);
  GError *error = NULL;
  SoupWebsocketConnection *conn;

  conn = soup_session_websocket_connect_finish(
      SOUP_SESSION(source), res, &error);

  if (error) {
    GST_ERROR_OBJECT(self, "WebSocket connection failed: %s", error->message);
    g_error_free(error);
    if (self->loop)
      g_main_loop_quit(self->loop);
    return;
  }

  if (!conn) {
    GST_ERROR_OBJECT(self, "WebSocket connection returned NULL without error");
    if (self->loop)
      g_main_loop_quit(self->loop);
    return;
  }

  GST_INFO_OBJECT(self, "WebSocket %sconnected to %s (attempt %u)", 
      (self->reconnect_count > 0 ? "re" : ""), self->uri, self->reconnect_count);
  GST_DEBUG_OBJECT(self, "Connection state: %d",
      soup_websocket_connection_get_state(conn));

  g_signal_connect(conn, "message",
      G_CALLBACK(on_websocket_message), self);
  g_signal_connect(conn, "error",
      G_CALLBACK(on_websocket_error), self);
  g_signal_connect(conn, "closed",
      G_CALLBACK(on_websocket_closed), self);

  g_mutex_lock(&self->state_lock);
  self->ws_conn = conn;
  self->connected = TRUE;
  g_cond_signal(&self->connect_cond);
  g_mutex_unlock(&self->state_lock);

  gst_websocket_transceiver_flush_queue(self);
}

static gpointer
gst_websocket_transceiver_ws_thread(gpointer user_data)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(user_data);

  GST_DEBUG_OBJECT(self, "WebSocket thread started");

  self->context = g_main_context_new();
  g_main_context_push_thread_default(self->context);

  while (self->ws_thread_running && self->reconnect_enabled && (self->max_reconnects == 0 || self->reconnect_count < self->max_reconnects)) {
    self->session = soup_session_new();
    self->loop = g_main_loop_new(self->context, FALSE);

    SoupMessage *msg = soup_message_new(SOUP_METHOD_GET, self->uri);
    if (!msg) {
      GST_ERROR_OBJECT(self, "Failed to create SoupMessage for URI: %s", self->uri);
      g_object_unref(self->session);
      self->session = NULL;
      g_main_loop_unref(self->loop);
      self->loop = NULL;
      break;
    }

    gchar *protocols[] = {NULL};
    GST_INFO_OBJECT(self, "Connecting to WebSocket URI: %s", self->uri);
    soup_session_websocket_connect_async(self->session, msg, NULL, protocols, 0,
        NULL, on_websocket_connected, self);

    g_main_loop_run(self->loop);

    g_main_loop_unref(self->loop);
    self->loop = NULL;
    g_object_unref(self->session);
    self->session = NULL;

    // cleanup connection
    g_mutex_lock(&self->state_lock);
    if (self->ws_conn) {
      SoupWebsocketConnection *conn = self->ws_conn;
      self->ws_conn = NULL;
      self->connected = FALSE;
      g_mutex_unlock(&self->state_lock);
      SoupWebsocketState state = soup_websocket_connection_get_state(conn);
      if (state == SOUP_WEBSOCKET_STATE_OPEN) {
        soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
      }
      g_object_unref(conn);
    } else {
      g_mutex_unlock(&self->state_lock);
    }

    // backoff for next attempt (skip if shutting down)
    if (!self->ws_thread_running)
      break;

    self->reconnect_count++;
    guint backoff = self->current_backoff_ms > 0 ?
                    MIN(self->current_backoff_ms * 2, self->max_backoff_ms) : self->initial_reconnect_delay_ms;
    self->current_backoff_ms = backoff;
    GST_INFO_OBJECT(self, "Reconnection attempt %u/%u failed, backoff %u ms",
                    self->reconnect_count, self->max_reconnects, backoff);
    g_usleep(backoff * G_TIME_SPAN_MILLISECOND);
  }

  g_main_context_pop_thread_default(self->context);
  g_main_context_unref(self->context);
  self->context = NULL;

  GST_DEBUG_OBJECT(self, "WebSocket thread stopped");
  return NULL;
}

static gpointer
gst_websocket_transceiver_output_thread(gpointer user_data)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(user_data);
  GstClock *clock;
  GstClockTime next_output_time;
  GstClockTime now;

  GST_DEBUG_OBJECT(self, "Output thread started");

  gchar *stream_id = gst_pad_create_stream_id(self->srcpad, GST_ELEMENT(self), "websocket");
  gst_pad_push_event(self->srcpad, gst_event_new_stream_start(stream_id));
  g_free(stream_id);

  gboolean caps_pushed = FALSE;
  gboolean segment_pushed = FALSE;
  gboolean timing_initialized = FALSE;
  gboolean initial_buffering = (self->initial_buffer_count > 0);

  clock = NULL;
  next_output_time = 0;

  while (self->output_thread_running) {
    GstBuffer *buffer = NULL;
    GstFlowReturn ret;

    if (!timing_initialized) {
      clock = gst_element_get_clock(GST_ELEMENT(self));
      if (clock) {
        g_mutex_lock(&self->output_lock);
        self->base_timestamp = gst_clock_get_time(clock);
        self->next_timestamp = 0;
        self->first_timestamp_set = TRUE;
        next_output_time = self->base_timestamp + self->frame_duration;
        g_mutex_unlock(&self->output_lock);
        timing_initialized = TRUE;
        GST_DEBUG_OBJECT(self, "Timing initialized, base_timestamp: %" GST_TIME_FORMAT,
            GST_TIME_ARGS(self->base_timestamp));
      } else {
        gint64 wait_until = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
        g_mutex_lock(&self->output_lock);
        g_cond_wait_until(&self->output_cond, &self->output_lock, wait_until);
        g_mutex_unlock(&self->output_lock);
        continue;
      }
    }

    // wait for initial buffering to avoid audio clicks
    if (initial_buffering && self->initial_buffer_count > 0) {
      g_mutex_lock(&self->queue_lock);
      guint queue_len = g_queue_get_length(self->recv_queue);

      if (queue_len < self->initial_buffer_count) {
        GST_DEBUG_OBJECT(self, "Initial buffering: %u/%u buffers",
            queue_len, self->initial_buffer_count);
        gint64 wait_until = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
        g_cond_wait_until(&self->queue_cond, &self->queue_lock, wait_until);
        g_mutex_unlock(&self->queue_lock);
        continue;
      } else {
        initial_buffering = FALSE;
        GST_INFO_OBJECT(self, "Initial buffering complete, starting playback with %u buffers",
            queue_len);
        g_mutex_unlock(&self->queue_lock);
      }
    }

    if (!caps_pushed) {
      g_mutex_lock(&self->state_lock);
      if (!self->caps_ready && self->output_thread_running) {
        gint64 wait_until = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
        g_cond_wait_until(&self->caps_cond, &self->state_lock, wait_until);
      }
      g_mutex_unlock(&self->state_lock);

      GstCaps *caps = gst_pad_get_current_caps(self->srcpad);
      if (caps) {
        gst_pad_push_event(self->srcpad, gst_event_new_caps(caps));
        gst_caps_unref(caps);
        caps_pushed = TRUE;
        GST_DEBUG_OBJECT(self, "Caps event pushed");
      } else {
        continue;
      }
    }

    gboolean force_segment = FALSE;
    g_mutex_lock(&self->output_lock);
    if (self->need_segment) {
        force_segment = TRUE;
        self->need_segment = FALSE;
    }
    g_mutex_unlock(&self->output_lock);

    if ((!segment_pushed || force_segment) && caps_pushed) {
      GstSegment segment;
      gst_segment_init(&segment, GST_FORMAT_TIME);
      gst_pad_push_event(self->srcpad, gst_event_new_segment(&segment));
      segment_pushed = TRUE;
      GST_DEBUG_OBJECT(self, "Segment event pushed");
    }

    g_mutex_lock(&self->state_lock);
    if (self->eos_sent) {
      g_mutex_unlock(&self->state_lock);
      GST_INFO_OBJECT(self, "EOS sent, stopping output thread");
      break;
    }
    g_mutex_unlock(&self->state_lock);

    now = gst_clock_get_time(clock);
    if (now < next_output_time) {
      gint64 wait_ns = next_output_time - now;
      gint64 wait_until = g_get_monotonic_time() + (wait_ns / 1000);
      g_mutex_lock(&self->output_lock);
      g_cond_wait_until(&self->output_cond, &self->output_lock, wait_until);
      g_mutex_unlock(&self->output_lock);
      if (!self->output_thread_running) {
        break;
      }
    }

    g_mutex_lock(&self->queue_lock);
    if (!g_queue_is_empty(self->recv_queue)) {
      buffer = g_queue_pop_head(self->recv_queue);
      GST_DEBUG_OBJECT(self, "Popped buffer from queue, %u remaining",
          g_queue_get_length(self->recv_queue));
    }
    g_mutex_unlock(&self->queue_lock);

    if (!buffer) {
      gboolean should_send_eos = FALSE;

      g_mutex_lock(&self->state_lock);
      if (!self->connected && !self->eos_sent) {
        self->eos_sent = TRUE;
        should_send_eos = TRUE;
      }
      g_mutex_unlock(&self->state_lock);

      if (should_send_eos) {
        GST_INFO_OBJECT(self, "Queue drained and WebSocket closed, sending EOS");
        gst_pad_push_event(self->srcpad, gst_event_new_eos());
        break;
      }

      // still advance timestamps to maintain continuity
      GST_LOG_OBJECT(self, "No data available, skipping");
      g_mutex_lock(&self->output_lock);
      self->next_timestamp += self->frame_duration;
      g_mutex_unlock(&self->output_lock);
      next_output_time += self->frame_duration;
      continue;
    }

    g_mutex_lock(&self->output_lock);
    GST_BUFFER_PTS(buffer) = self->base_timestamp + self->next_timestamp;
    GST_BUFFER_DURATION(buffer) = self->frame_duration;
    self->next_timestamp += self->frame_duration;
    g_mutex_unlock(&self->output_lock);

    ret = gst_pad_push(self->srcpad, buffer);
    if (ret != GST_FLOW_OK) {
      GST_WARNING_OBJECT(self, "Error pushing buffer: %s", gst_flow_get_name(ret));
      // Ignore FLUSHING if we are still running (barge-in case)
      if ((ret == GST_FLOW_FLUSHING && !self->output_thread_running) || ret == GST_FLOW_EOS) {
        break;
      }
    }

    next_output_time += self->frame_duration;
  }

  if (clock) {
    gst_object_unref(clock);
  }
  GST_DEBUG_OBJECT(self, "Output thread stopped");
  return NULL;
}

static GstFlowReturn
gst_websocket_transceiver_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(parent);
  GstMapInfo map;
  SoupWebsocketConnection *conn = NULL;

  g_mutex_lock(&self->state_lock);
  if (!self->connected || !self->ws_conn) {
    g_mutex_unlock(&self->state_lock);
    GST_WARNING_OBJECT(self, "WebSocket not connected, dropping buffer");
    gst_buffer_unref(buffer);
    return GST_FLOW_OK;
  }
  conn = g_object_ref(self->ws_conn);
  g_mutex_unlock(&self->state_lock);

  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    GBytes *bytes = g_bytes_new(map.data, map.size);

    GST_LOG_OBJECT(self, "Sending %zu bytes over WebSocket", map.size);

    soup_websocket_connection_send_binary(conn,
        g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));

    g_bytes_unref(bytes);
    gst_buffer_unmap(buffer, &map);
  }

  g_object_unref(conn);
  gst_buffer_unref(buffer);
  return GST_FLOW_OK;
}

static GstStateChangeReturn
gst_websocket_transceiver_change_state(GstElement *element, GstStateChange transition)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(element);
  GstStateChangeReturn ret;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      self->reconnect_count = 0;
      self->current_backoff_ms = 0;
      if (!self->uri) {
        GST_ERROR_OBJECT(self, "No Websocket URI set");
        return GST_STATE_CHANGE_FAILURE;
      }

      self->ws_thread_running = TRUE;
      self->ws_thread = g_thread_new("websocket-thread",
          gst_websocket_transceiver_ws_thread, self);

      {
        gint64 end_time = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        g_mutex_lock(&self->state_lock);
        while (!self->connected) {
          if (!g_cond_wait_until(&self->connect_cond, &self->state_lock, end_time)) {
            g_mutex_unlock(&self->state_lock);
            GST_WARNING_OBJECT(self, "WebSocket connection timeout, continuing anyway");
            break;
          }
        }
        if (self->connected) {
          g_mutex_unlock(&self->state_lock);
          GST_INFO_OBJECT(self, "WebSocket connection established");
        }
      }
      break;

    case GST_STATE_CHANGE_READY_TO_PAUSED:
      g_mutex_lock(&self->state_lock);
      self->eos_sent = FALSE;
      self->caps_ready = FALSE;
      g_mutex_unlock(&self->state_lock);

      self->output_thread_running = TRUE;
      self->output_thread = g_thread_new("output-thread",
          gst_websocket_transceiver_output_thread, self);
      break;

    default:
      break;
  }

  ret = GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      ret = GST_STATE_CHANGE_NO_PREROLL;
      break;

    case GST_STATE_CHANGE_PAUSED_TO_READY:
      self->output_thread_running = FALSE;

      g_mutex_lock(&self->queue_lock);
      g_cond_broadcast(&self->queue_cond);
      g_mutex_unlock(&self->queue_lock);

      g_mutex_lock(&self->state_lock);
      g_cond_broadcast(&self->caps_cond);
      g_mutex_unlock(&self->state_lock);

      g_mutex_lock(&self->output_lock);
      g_cond_broadcast(&self->output_cond);
      g_mutex_unlock(&self->output_lock);

      if (self->output_thread) {
        g_thread_join(self->output_thread);
        self->output_thread = NULL;
      }

      self->first_timestamp_set = FALSE;
      self->next_timestamp = 0;
      self->caps_ready = FALSE;
      break;

    case GST_STATE_CHANGE_READY_TO_NULL:
      self->ws_thread_running = FALSE;
      if (self->loop) {
        g_main_loop_quit(self->loop);
      }
      if (self->ws_thread) {
        g_thread_join(self->ws_thread);
        self->ws_thread = NULL;
      }

      g_mutex_lock(&self->queue_lock);
      g_queue_free_full(self->recv_queue, (GDestroyNotify)gst_buffer_unref);
      self->recv_queue = g_queue_new();
      g_mutex_unlock(&self->queue_lock);

      g_mutex_lock(&self->state_lock);
      self->connected = FALSE;
      self->eos_sent = FALSE;
      g_mutex_unlock(&self->state_lock);
      break;

    default:
      break;
  }

  return ret;
}

