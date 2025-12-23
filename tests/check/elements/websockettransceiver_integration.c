#include <gst/check/gstcheck.h>

#define TEST_WS_URI "ws://127.0.0.1:9999"

GST_START_TEST(test_connection)
{
  GstElement *element;
  GstStateChangeReturn ret;

  element = gst_element_factory_make("websockettransceiver", NULL);
  fail_unless(element != NULL);

  g_object_set(element,
      "uri", TEST_WS_URI,
      "sample-rate", 16000,
      "channels", 1,
      NULL);

  ret = gst_element_set_state(element, GST_STATE_PLAYING);
  fail_unless(ret == GST_STATE_CHANGE_SUCCESS || ret == GST_STATE_CHANGE_NO_PREROLL,
      "State change should succeed (got %d)", ret);

  g_usleep(1000000);

  gst_element_set_state(element, GST_STATE_NULL);
  gst_object_unref(element);
}
GST_END_TEST;

GST_START_TEST(test_send_data)
{
  GstElement *element;
  GstPad *sink_pad;
  GstBuffer *buffer;
  GstCaps *caps;
  GstSegment segment;
  GstMapInfo map;
  GstFlowReturn flow_ret;
  gint16 *samples;
  gint i;

  element = gst_element_factory_make("websockettransceiver", NULL);
  fail_unless(element != NULL);

  g_object_set(element,
      "uri", TEST_WS_URI,
      "sample-rate", 16000,
      "channels", 1,
      "frame-duration-ms", 20,
      NULL);

  sink_pad = gst_element_get_static_pad(element, "sink");
  fail_unless(sink_pad != NULL);

  caps = gst_caps_new_simple("audio/x-raw",
      "format", G_TYPE_STRING, "S16LE",
      "rate", G_TYPE_INT, 16000,
      "channels", G_TYPE_INT, 1,
      "layout", G_TYPE_STRING, "interleaved",
      NULL);

  gst_element_set_state(element, GST_STATE_PLAYING);
  g_usleep(1000000);

  fail_unless(gst_pad_send_event(sink_pad, gst_event_new_stream_start("test")));
  fail_unless(gst_pad_send_event(sink_pad, gst_event_new_caps(caps)));
  gst_segment_init(&segment, GST_FORMAT_TIME);
  fail_unless(gst_pad_send_event(sink_pad, gst_event_new_segment(&segment)));

  buffer = gst_buffer_new_allocate(NULL, 640, NULL);
  fail_unless(buffer != NULL);

  fail_unless(gst_buffer_map(buffer, &map, GST_MAP_WRITE));
  samples = (gint16 *)map.data;
  for (i = 0; i < 320; i++) {
    samples[i] = (gint16)(i * 100);
  }
  gst_buffer_unmap(buffer, &map);

  GST_BUFFER_PTS(buffer) = 0;
  GST_BUFFER_DURATION(buffer) = GST_MSECOND * 20;

  flow_ret = gst_pad_chain(sink_pad, buffer);
  fail_unless(flow_ret == GST_FLOW_OK, "Chain should return OK (got %d)", flow_ret);

  gst_caps_unref(caps);
  gst_object_unref(sink_pad);
  gst_element_set_state(element, GST_STATE_NULL);
  gst_object_unref(element);
}
GST_END_TEST;

GST_START_TEST(test_send_multiple_buffers)
{
  GstElement *element;
  GstPad *sink_pad;
  GstBuffer *buffer;
  GstCaps *caps;
  GstSegment segment;
  GstFlowReturn flow_ret;
  gint i;

  element = gst_element_factory_make("websockettransceiver", NULL);
  fail_unless(element != NULL);

  g_object_set(element,
      "uri", TEST_WS_URI,
      "sample-rate", 16000,
      "channels", 1,
      "frame-duration-ms", 20,
      NULL);

  sink_pad = gst_element_get_static_pad(element, "sink");
  fail_unless(sink_pad != NULL);

  caps = gst_caps_new_simple("audio/x-raw",
      "format", G_TYPE_STRING, "S16LE",
      "rate", G_TYPE_INT, 16000,
      "channels", G_TYPE_INT, 1,
      "layout", G_TYPE_STRING, "interleaved",
      NULL);

  gst_element_set_state(element, GST_STATE_PLAYING);
  g_usleep(1000000);

  gst_pad_send_event(sink_pad, gst_event_new_stream_start("test"));
  gst_pad_send_event(sink_pad, gst_event_new_caps(caps));
  gst_segment_init(&segment, GST_FORMAT_TIME);
  gst_pad_send_event(sink_pad, gst_event_new_segment(&segment));

  for (i = 0; i < 10; i++) {
    buffer = gst_buffer_new_allocate(NULL, 640, NULL);
    GST_BUFFER_PTS(buffer) = i * GST_MSECOND * 20;
    GST_BUFFER_DURATION(buffer) = GST_MSECOND * 20;

    flow_ret = gst_pad_chain(sink_pad, buffer);
    fail_unless(flow_ret == GST_FLOW_OK, "Buffer %d: chain should return OK", i);
  }

  gst_caps_unref(caps);
  gst_object_unref(sink_pad);
  gst_element_set_state(element, GST_STATE_NULL);
  gst_object_unref(element);
}
GST_END_TEST;

GST_START_TEST(test_barge_in_clear)
{
  GstElement *element;
  GstPad *sink_pad;
  GstBuffer *buffer;
  GstCaps *caps;
  GstSegment segment;
  GstFlowReturn flow_ret;
  gint i;

  element = gst_element_factory_make("websockettransceiver", NULL);
  fail_unless(element != NULL);

  g_object_set(element,
      "uri", TEST_WS_URI,
      "sample-rate", 16000,
      "channels", 1,
      "frame-duration-ms", 20,
      NULL);

  sink_pad = gst_element_get_static_pad(element, "sink");
  fail_unless(sink_pad != NULL);

  caps = gst_caps_new_simple("audio/x-raw",
      "format", G_TYPE_STRING, "S16LE",
      "rate", G_TYPE_INT, 16000,
      "channels", G_TYPE_INT, 1,
      "layout", G_TYPE_STRING, "interleaved",
      NULL);

  gst_element_set_state(element, GST_STATE_PLAYING);
  g_usleep(1000000);

  gst_pad_send_event(sink_pad, gst_event_new_stream_start("test"));
  gst_pad_send_event(sink_pad, gst_event_new_caps(caps));
  gst_segment_init(&segment, GST_FORMAT_TIME);
  gst_pad_send_event(sink_pad, gst_event_new_segment(&segment));

  for (i = 0; i < 5; i++) {
    buffer = gst_buffer_new_allocate(NULL, 640, NULL);
    GST_BUFFER_PTS(buffer) = i * GST_MSECOND * 20;
    GST_BUFFER_DURATION(buffer) = GST_MSECOND * 20;

    flow_ret = gst_pad_chain(sink_pad, buffer);
    fail_unless(flow_ret == GST_FLOW_OK || flow_ret == GST_FLOW_FLUSHING,
        "Buffer %d: chain returned unexpected %d", i, flow_ret);

    g_usleep(50000);
  }

  g_usleep(500000);

  gst_caps_unref(caps);
  gst_object_unref(sink_pad);
  gst_element_set_state(element, GST_STATE_NULL);
  gst_object_unref(element);
}
GST_END_TEST;

static Suite *
websockettransceiver_harness_suite(void)
{
  Suite *s = suite_create("websockettransceiver_integration");
  TCase *tc = tcase_create("integration");

  tcase_set_timeout(tc, 30);

  suite_add_tcase(s, tc);
  tcase_add_test(tc, test_connection);
  tcase_add_test(tc, test_send_data);
  tcase_add_test(tc, test_send_multiple_buffers);
  tcase_add_test(tc, test_barge_in_clear);

  return s;
}

GST_CHECK_MAIN(websockettransceiver_harness);
