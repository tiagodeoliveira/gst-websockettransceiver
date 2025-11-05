#include "gstwebsockettransceiver.h"
#include <string.h>
#include <gst/audio/audio.h>

GST_DEBUG_CATEGORY_STATIC(gst_websocket_transceiver_debug);
#define GST_CAT_DEFAULT gst_websocket_transceiver_debug

/* Properties */
enum
{
  PROP_0,
  PROP_URI,
  PROP_SAMPLE_RATE,
  PROP_CHANNELS,
  PROP_FRAME_DURATION_MS,
  PROP_MAX_QUEUE_SIZE,
  PROP_INITIAL_BUFFER_COUNT,
};

/* Default values */
#define DEFAULT_URI NULL
#define DEFAULT_SAMPLE_RATE 16000
#define DEFAULT_CHANNELS 1
#define DEFAULT_FRAME_DURATION_MS 250
#define DEFAULT_MAX_QUEUE_SIZE 100
#define DEFAULT_INITIAL_BUFFER_COUNT 3  /* Wait for 3 buffers before starting playback */

/* Pad templates - accept any audio format (codec agnostic) */
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

/* Forward declarations */
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

/* Class initialization */
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

/* Instance initialization */
static void
gst_websocket_transceiver_init(GstWebSocketTransceiver *self)
{
  /* Create pads */
  self->sinkpad = gst_pad_new_from_static_template(&sink_template, "sink");
  gst_pad_set_chain_function(self->sinkpad, gst_websocket_transceiver_chain);
  gst_pad_set_event_function(self->sinkpad, gst_websocket_transceiver_sink_event);
  gst_element_add_pad(GST_ELEMENT(self), self->sinkpad);

  self->srcpad = gst_pad_new_from_static_template(&src_template, "src");
  gst_pad_set_query_function(self->srcpad, gst_websocket_transceiver_src_query);
  gst_element_add_pad(GST_ELEMENT(self), self->srcpad);

  /* Initialize properties */
  self->uri = NULL;
  self->sample_rate = DEFAULT_SAMPLE_RATE;
  self->channels = DEFAULT_CHANNELS;
  self->frame_duration_ms = DEFAULT_FRAME_DURATION_MS;
  self->max_queue_size = DEFAULT_MAX_QUEUE_SIZE;
  self->initial_buffer_count = DEFAULT_INITIAL_BUFFER_COUNT;

  /* Calculate audio parameters - will be set by caps negotiation */
  self->bytes_per_sample = 0;  /* Will be set during caps negotiation */
  self->frame_size_bytes = 0;   /* Will be calculated after caps negotiation */
  self->frame_duration = self->frame_duration_ms * GST_MSECOND;

  /* Initialize timing */
  self->base_timestamp = GST_CLOCK_TIME_NONE;
  self->next_timestamp = 0;
  self->first_timestamp_set = FALSE;

  /* Initialize queue */
  self->recv_queue = g_queue_new();
  g_mutex_init(&self->queue_lock);

  /* Initialize output thread sync */
  g_mutex_init(&self->output_lock);
  g_cond_init(&self->output_cond);
  self->output_thread_running = FALSE;

  /* Initialize state */
  self->connected = FALSE;
  self->eos_sent = FALSE;
  g_mutex_init(&self->state_lock);

  /* WebSocket */
  self->session = NULL;
  self->ws_conn = NULL;
  self->context = NULL;
  self->loop = NULL;
  self->ws_thread = NULL;
  self->output_thread = NULL;

  /* Mark as live source - produces data in real-time */
  GST_OBJECT_FLAG_SET(self, GST_ELEMENT_FLAG_SOURCE);
}

/* Finalization */
static void
gst_websocket_transceiver_finalize(GObject *object)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(object);

  g_free(self->uri);

  /* Clear queue */
  g_mutex_lock(&self->queue_lock);
  g_queue_free_full(self->recv_queue, (GDestroyNotify)gst_buffer_unref);
  g_mutex_unlock(&self->queue_lock);

  g_mutex_clear(&self->queue_lock);
  g_mutex_clear(&self->output_lock);
  g_cond_clear(&self->output_cond);
  g_mutex_clear(&self->state_lock);

  G_OBJECT_CLASS(parent_class)->finalize(object);
}

/* Property setters/getters */
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
      /* Recalculate frame size */
      self->frame_size_bytes = (self->sample_rate * self->bytes_per_sample *
                                self->channels * self->frame_duration_ms) / 1000;
      break;
    case PROP_CHANNELS:
      self->channels = g_value_get_uint(value);
      /* Recalculate frame size */
      self->frame_size_bytes = (self->sample_rate * self->bytes_per_sample *
                                self->channels * self->frame_duration_ms) / 1000;
      break;
    case PROP_FRAME_DURATION_MS:
      self->frame_duration_ms = g_value_get_uint(value);
      /* Recalculate frame size and duration */
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
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/* Src pad query handler */
static gboolean
gst_websocket_transceiver_src_query(GstPad *pad, GstObject *parent, GstQuery *query)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(parent);
  gboolean ret = TRUE;

  switch (GST_QUERY_TYPE(query)) {
    case GST_QUERY_LATENCY:
    {
      /* Report as live source with latency equal to frame duration */
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

/* Sink pad caps negotiation */
static gboolean
gst_websocket_transceiver_sink_setcaps(GstWebSocketTransceiver *self, GstCaps *caps)
{
  GstStructure *structure;
  const gchar *format_name;
  gint rate = 0, channels = 0;

  structure = gst_caps_get_structure(caps, 0);
  format_name = gst_structure_get_name(structure);

  /* Extract rate and channels (common to all audio formats) */
  if (!gst_structure_get_int(structure, "rate", &rate) ||
      !gst_structure_get_int(structure, "channels", &channels)) {
    GST_ERROR_OBJECT(self, "Caps missing rate or channels: %" GST_PTR_FORMAT, caps);
    return FALSE;
  }

  self->sample_rate = rate;
  self->channels = channels;

  /* Determine bytes per sample based on format */
  if (g_str_equal(format_name, "audio/x-raw")) {
    /* For PCM formats, parse with GstAudioInfo */
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
    /* μ-law and A-law are 1 byte per sample */
    self->bytes_per_sample = 1;
    GST_INFO_OBJECT(self, "Compressed audio format: %s, 1 byte/sample", format_name);
  } else {
    /* Unknown format - assume 1 byte per sample for safety */
    GST_WARNING_OBJECT(self, "Unknown audio format %s, assuming 1 byte/sample", format_name);
    self->bytes_per_sample = 1;
  }

  /* Recalculate frame size based on timing */
  self->frame_size_bytes = (self->sample_rate * self->bytes_per_sample *
                            self->channels * self->frame_duration_ms) / 1000;
  self->frame_duration = self->frame_duration_ms * GST_MSECOND;

  GST_INFO_OBJECT(self, "Caps negotiated: format=%s, rate=%d Hz, channels=%d, "
      "bytes_per_sample=%d, frame_size=%d bytes (%.1f ms)",
      format_name, self->sample_rate, self->channels,
      self->bytes_per_sample, self->frame_size_bytes,
      (float)self->frame_duration_ms);

  /* Set same caps on src pad - codec agnostic passthrough */
  if (!gst_pad_set_caps(self->srcpad, caps)) {
    GST_ERROR_OBJECT(self, "Failed to set caps on src pad");
    return FALSE;
  }

  return TRUE;
}

/* Sink pad event handler */
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
      GST_INFO_OBJECT(self, "Received EOS on sink pad (input stream ended) - ignoring, will send EOS when WebSocket closes");
      /* For a bidirectional element, sink EOS is independent from src EOS.
       * We only send EOS on src pad when the WebSocket connection closes.
       * Consume this event and don't propagate it. */
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

/* WebSocket message received callback */
static void
on_websocket_message(SoupWebsocketConnection *conn, gint type, GBytes *message,
    gpointer user_data)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(user_data);
  GstBuffer *buffer;
  gconstpointer data;
  gsize size;

  if (type != SOUP_WEBSOCKET_DATA_BINARY) {
    GST_WARNING_OBJECT(self, "Received non-binary WebSocket message");
    return;
  }

  data = g_bytes_get_data(message, &size);

  GST_DEBUG_OBJECT(self, "Received WebSocket message: %zu bytes", size);

  /* Create GStreamer buffer */
  buffer = gst_buffer_new_allocate(NULL, size, NULL);
  gst_buffer_fill(buffer, 0, data, size);

  /* Add to queue */
  g_mutex_lock(&self->queue_lock);

  /* Drop old frames if queue is full */
  while (g_queue_get_length(self->recv_queue) >= self->max_queue_size) {
    GstBuffer *dropped = g_queue_pop_head(self->recv_queue);
    gst_buffer_unref(dropped);
    GST_WARNING_OBJECT(self, "Queue full (%u), dropped old buffer", self->max_queue_size);
  }

  g_queue_push_tail(self->recv_queue, buffer);
  GST_DEBUG_OBJECT(self, "Queued buffer, queue length: %u",
      g_queue_get_length(self->recv_queue));

  g_mutex_unlock(&self->queue_lock);
}

/* WebSocket error callback */
static void
on_websocket_error(SoupWebsocketConnection *conn, GError *error, gpointer user_data)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(user_data);
  GST_ERROR_OBJECT(self, "WebSocket error: %s", error ? error->message : "unknown");
}

/* WebSocket closed callback */
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

  /* Just mark as disconnected - let output thread drain queue before sending EOS */
  g_mutex_lock(&self->state_lock);
  self->connected = FALSE;
  g_mutex_unlock(&self->state_lock);

  GST_INFO_OBJECT(self, "WebSocket disconnected, output thread will drain queue and send EOS");
}

/* WebSocket connection established callback */
static void
on_websocket_connected(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(user_data);
  GError *error = NULL;

  self->ws_conn = soup_session_websocket_connect_finish(
      SOUP_SESSION(source), res, &error);

  if (error) {
    GST_ERROR_OBJECT(self, "WebSocket connection failed: %s", error->message);
    g_error_free(error);
    return;
  }

  if (!self->ws_conn) {
    GST_ERROR_OBJECT(self, "WebSocket connection returned NULL without error");
    return;
  }

  GST_INFO_OBJECT(self, "WebSocket connected to %s", self->uri);
  GST_DEBUG_OBJECT(self, "Connection state: %d",
      soup_websocket_connection_get_state(self->ws_conn));

  g_signal_connect(self->ws_conn, "message",
      G_CALLBACK(on_websocket_message), self);
  g_signal_connect(self->ws_conn, "error",
      G_CALLBACK(on_websocket_error), self);
  g_signal_connect(self->ws_conn, "closed",
      G_CALLBACK(on_websocket_closed), self);

  g_mutex_lock(&self->state_lock);
  self->connected = TRUE;
  g_mutex_unlock(&self->state_lock);
}

/* WebSocket thread */
static gpointer
gst_websocket_transceiver_ws_thread(gpointer user_data)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(user_data);
  SoupMessage *msg;

  GST_DEBUG_OBJECT(self, "WebSocket thread started");

  self->context = g_main_context_new();
  g_main_context_push_thread_default(self->context);

  self->session = soup_session_new();
  self->loop = g_main_loop_new(self->context, FALSE);

  /* Connect to WebSocket */
  GST_INFO_OBJECT(self, "Connecting to WebSocket URI: %s", self->uri);
  msg = soup_message_new(SOUP_METHOD_GET, self->uri);
  if (!msg) {
    GST_ERROR_OBJECT(self, "Failed to create SoupMessage for URI: %s", self->uri);
    return NULL;
  }

  /* Use empty protocols array instead of NULL */
  gchar *protocols[] = {NULL};
  soup_session_websocket_connect_async(self->session, msg, NULL, protocols, 0,
      NULL, on_websocket_connected, self);

  /* Run event loop */
  g_main_loop_run(self->loop);

  /* Cleanup */
  if (self->ws_conn) {
    SoupWebsocketState state = soup_websocket_connection_get_state(self->ws_conn);
    if (state == SOUP_WEBSOCKET_STATE_OPEN) {
      GST_DEBUG_OBJECT(self, "Closing WebSocket connection");
      soup_websocket_connection_close(self->ws_conn,
          SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
    } else {
      GST_DEBUG_OBJECT(self, "WebSocket already closed (state: %d)", state);
    }
    g_object_unref(self->ws_conn);
    self->ws_conn = NULL;
  }

  g_object_unref(self->session);
  self->session = NULL;

  g_main_loop_unref(self->loop);
  self->loop = NULL;

  g_main_context_pop_thread_default(self->context);
  g_main_context_unref(self->context);
  self->context = NULL;

  GST_DEBUG_OBJECT(self, "WebSocket thread stopped");
  return NULL;
}

/* Output thread - pushes buffers at configured frame rate */
static gpointer
gst_websocket_transceiver_output_thread(gpointer user_data)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(user_data);
  GstClock *clock;
  GstClockTime next_output_time;
  GstClockTime now;

  GST_DEBUG_OBJECT(self, "Output thread started");

  /* Send stream-start event */
  gchar *stream_id = gst_pad_create_stream_id(self->srcpad, GST_ELEMENT(self), "websocket");
  gst_pad_push_event(self->srcpad, gst_event_new_stream_start(stream_id));
  g_free(stream_id);

  /* These will be pushed once available */
  gboolean caps_pushed = FALSE;
  gboolean segment_pushed = FALSE;
  gboolean timing_initialized = FALSE;
  gboolean initial_buffering = (self->initial_buffer_count > 0);

  clock = NULL;
  next_output_time = 0;

  while (self->output_thread_running) {
    GstBuffer *buffer = NULL;
    GstFlowReturn ret;

    /* Lazy initialization of clock, caps, and segment */
    if (!timing_initialized) {
      clock = gst_element_get_clock(GST_ELEMENT(self));
      if (clock) {
        g_mutex_lock(&self->output_lock);
        self->base_timestamp = gst_clock_get_time(clock);
        self->next_timestamp = 0;
        self->first_timestamp_set = TRUE;
        /* Start output time one frame duration in the future for smooth startup */
        next_output_time = self->base_timestamp + self->frame_duration;
        g_mutex_unlock(&self->output_lock);
        timing_initialized = TRUE;
        GST_DEBUG_OBJECT(self, "Timing initialized, base_timestamp: %" GST_TIME_FORMAT,
            GST_TIME_ARGS(self->base_timestamp));
      } else {
        /* Clock not available yet, sleep and retry */
        g_usleep(10000);  /* 10ms */
        continue;
      }
    }

    /* Wait for initial buffering - accumulate some audio before starting playback */
    if (initial_buffering && self->initial_buffer_count > 0) {
      g_mutex_lock(&self->queue_lock);
      guint queue_len = g_queue_get_length(self->recv_queue);
      g_mutex_unlock(&self->queue_lock);

      if (queue_len < self->initial_buffer_count) {
        GST_DEBUG_OBJECT(self, "Initial buffering: %u/%u buffers",
            queue_len, self->initial_buffer_count);
        g_usleep(50000);  /* 50ms */
        continue;
      } else {
        initial_buffering = FALSE;
        GST_INFO_OBJECT(self, "Initial buffering complete, starting playback with %u buffers",
            queue_len);
      }
    }

    if (!caps_pushed) {
      GstCaps *caps = gst_pad_get_current_caps(self->srcpad);
      if (caps) {
        gst_pad_push_event(self->srcpad, gst_event_new_caps(caps));
        gst_caps_unref(caps);
        caps_pushed = TRUE;
        GST_DEBUG_OBJECT(self, "Caps event pushed");
      }
    }

    if (!segment_pushed && caps_pushed) {
      GstSegment segment;
      gst_segment_init(&segment, GST_FORMAT_TIME);
      gst_pad_push_event(self->srcpad, gst_event_new_segment(&segment));
      segment_pushed = TRUE;
      GST_DEBUG_OBJECT(self, "Segment event pushed");
    }

    /* Skip this iteration if not fully initialized */
    if (!caps_pushed || !segment_pushed) {
      g_usleep(10000);  /* 10ms */
      continue;
    }

    /* Check if EOS was sent (WebSocket closed) */
    g_mutex_lock(&self->state_lock);
    if (self->eos_sent) {
      g_mutex_unlock(&self->state_lock);
      GST_INFO_OBJECT(self, "EOS sent, stopping output thread");
      break;
    }
    g_mutex_unlock(&self->state_lock);

    /* Wait until next output time */
    now = gst_clock_get_time(clock);
    if (now < next_output_time) {
      g_usleep((next_output_time - now) / 1000);
    }

    /* Get data from queue */
    g_mutex_lock(&self->queue_lock);
    if (!g_queue_is_empty(self->recv_queue)) {
      buffer = g_queue_pop_head(self->recv_queue);
      GST_DEBUG_OBJECT(self, "Popped buffer from queue, %u remaining",
          g_queue_get_length(self->recv_queue));
    }
    g_mutex_unlock(&self->queue_lock);

    /* Check if connection closed and queue empty - time to send EOS */
    if (!buffer) {
      gboolean should_send_eos = FALSE;

      g_mutex_lock(&self->state_lock);
      if (!self->connected && !self->eos_sent) {
        /* Connection closed and no data in queue */
        self->eos_sent = TRUE;
        should_send_eos = TRUE;
      }
      g_mutex_unlock(&self->state_lock);

      if (should_send_eos) {
        GST_INFO_OBJECT(self, "Queue drained and WebSocket closed, sending EOS");
        gst_pad_push_event(self->srcpad, gst_event_new_eos());
        break;
      }

      /* No data available, skip this cycle */
      /* IMPORTANT: Still advance timestamps to maintain continuity */
      GST_LOG_OBJECT(self, "No data available, skipping");
      g_mutex_lock(&self->output_lock);
      self->next_timestamp += self->frame_duration;
      g_mutex_unlock(&self->output_lock);
      next_output_time += self->frame_duration;
      continue;
    }

    /* Set timestamp */
    g_mutex_lock(&self->output_lock);
    GST_BUFFER_PTS(buffer) = self->base_timestamp + self->next_timestamp;
    GST_BUFFER_DURATION(buffer) = self->frame_duration;
    self->next_timestamp += self->frame_duration;
    g_mutex_unlock(&self->output_lock);

    /* Push buffer */
    ret = gst_pad_push(self->srcpad, buffer);
    if (ret != GST_FLOW_OK) {
      GST_WARNING_OBJECT(self, "Error pushing buffer: %s", gst_flow_get_name(ret));
      if (ret == GST_FLOW_FLUSHING || ret == GST_FLOW_EOS) {
        break;
      }
    }

    next_output_time += self->frame_duration;
  }

  gst_object_unref(clock);
  GST_DEBUG_OBJECT(self, "Output thread stopped");
  return NULL;
}

/* Sink pad chain function */
static GstFlowReturn
gst_websocket_transceiver_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(parent);
  GstMapInfo map;
  GBytes *bytes;

  if (!self->connected || !self->ws_conn) {
    GST_WARNING_OBJECT(self, "WebSocket not connected, dropping buffer");
    gst_buffer_unref(buffer);
    return GST_FLOW_OK;
  }

  /* Extract data and send over WebSocket */
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    bytes = g_bytes_new(map.data, map.size);

    GST_LOG_OBJECT(self, "Sending %zu bytes over WebSocket", map.size);

    soup_websocket_connection_send_binary(self->ws_conn,
        g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));

    g_bytes_unref(bytes);
    gst_buffer_unmap(buffer, &map);
  }

  gst_buffer_unref(buffer);
  return GST_FLOW_OK;
}

/* State change handler */
static GstStateChangeReturn
gst_websocket_transceiver_change_state(GstElement *element, GstStateChange transition)
{
  GstWebSocketTransceiver *self = GST_WEBSOCKET_TRANSCEIVER(element);
  GstStateChangeReturn ret;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!self->uri) {
        GST_ERROR_OBJECT(self, "No URI set");
        return GST_STATE_CHANGE_FAILURE;
      }

      /* Start WebSocket thread */
      self->ws_thread = g_thread_new("websocket-thread",
          gst_websocket_transceiver_ws_thread, self);

      /* Wait a bit for connection */
      g_usleep(500000);  /* 500ms */
      break;

    case GST_STATE_CHANGE_READY_TO_PAUSED:
      /* Reset EOS flag for new stream */
      g_mutex_lock(&self->state_lock);
      self->eos_sent = FALSE;
      g_mutex_unlock(&self->state_lock);

      /* Start output thread */
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
      /* Live source - no preroll needed */
      ret = GST_STATE_CHANGE_NO_PREROLL;
      break;

    case GST_STATE_CHANGE_PAUSED_TO_READY:
      /* Stop output thread */
      self->output_thread_running = FALSE;
      if (self->output_thread) {
        g_thread_join(self->output_thread);
        self->output_thread = NULL;
      }

      /* Reset timing */
      self->first_timestamp_set = FALSE;
      self->next_timestamp = 0;
      break;

    case GST_STATE_CHANGE_READY_TO_NULL:
      /* Stop WebSocket thread */
      if (self->loop) {
        g_main_loop_quit(self->loop);
      }
      if (self->ws_thread) {
        g_thread_join(self->ws_thread);
        self->ws_thread = NULL;
      }

      /* Clear queue */
      g_mutex_lock(&self->queue_lock);
      g_queue_free_full(self->recv_queue, (GDestroyNotify)gst_buffer_unref);
      self->recv_queue = g_queue_new();
      g_mutex_unlock(&self->queue_lock);

      g_mutex_lock(&self->state_lock);
      self->connected = FALSE;
      self->eos_sent = FALSE;  /* Reset for next connection */
      g_mutex_unlock(&self->state_lock);
      break;

    default:
      break;
  }

  return ret;
}
