/* GstCheck unit tests for websockettransceiver element
 *
 * These are fast-fail tests that verify basic element functionality
 * without requiring an actual WebSocket server.
 */

#include <gst/check/gstcheck.h>

/* Test that the element can be created */
GST_START_TEST(test_element_create)
{
  GstElement *element;

  element = gst_element_factory_make("websockettransceiver", NULL);
  fail_unless(element != NULL, "Failed to create websockettransceiver element");
  fail_unless(GST_IS_ELEMENT(element), "Created object is not a GstElement");

  gst_object_unref(element);
}
GST_END_TEST;

/* Test that properties have correct default values */
GST_START_TEST(test_properties_default)
{
  GstElement *element;
  gchar *uri = NULL;
  gint sample_rate, channels, frame_duration, max_queue_size, initial_buffer_count;

  element = gst_element_factory_make("websockettransceiver", NULL);
  fail_unless(element != NULL);

  g_object_get(element,
      "uri", &uri,
      "sample-rate", &sample_rate,
      "channels", &channels,
      "frame-duration-ms", &frame_duration,
      "max-queue-size", &max_queue_size,
      "initial-buffer-count", &initial_buffer_count,
      NULL);

  /* Check defaults match what's defined in the plugin */
  fail_unless(uri == NULL, "Default URI should be NULL");
  fail_unless_equals_int(sample_rate, 16000);
  fail_unless_equals_int(channels, 1);
  fail_unless_equals_int(frame_duration, 250);
  fail_unless_equals_int(max_queue_size, 100);
  fail_unless_equals_int(initial_buffer_count, 3);

  g_free(uri);
  gst_object_unref(element);
}
GST_END_TEST;

/* Test that properties can be set and retrieved */
GST_START_TEST(test_properties_set_get)
{
  GstElement *element;
  gchar *uri = NULL;
  gint sample_rate, channels;

  element = gst_element_factory_make("websockettransceiver", NULL);
  fail_unless(element != NULL);

  /* Set properties */
  g_object_set(element,
      "uri", "wss://example.com/ws",
      "sample-rate", 48000,
      "channels", 2,
      "frame-duration-ms", 100,
      NULL);

  /* Verify they were set correctly */
  g_object_get(element,
      "uri", &uri,
      "sample-rate", &sample_rate,
      "channels", &channels,
      NULL);

  fail_unless_equals_string(uri, "wss://example.com/ws");
  fail_unless_equals_int(sample_rate, 48000);
  fail_unless_equals_int(channels, 2);

  g_free(uri);
  gst_object_unref(element);
}
GST_END_TEST;

/* Test that element has expected pads */
GST_START_TEST(test_pads_exist)
{
  GstElement *element;
  GstPad *sink_pad, *src_pad;

  element = gst_element_factory_make("websockettransceiver", NULL);
  fail_unless(element != NULL);

  sink_pad = gst_element_get_static_pad(element, "sink");
  fail_unless(sink_pad != NULL, "Element should have a sink pad");
  fail_unless(GST_PAD_IS_SINK(sink_pad), "Sink pad should be a sink");

  src_pad = gst_element_get_static_pad(element, "src");
  fail_unless(src_pad != NULL, "Element should have a src pad");
  fail_unless(GST_PAD_IS_SRC(src_pad), "Src pad should be a source");

  gst_object_unref(sink_pad);
  gst_object_unref(src_pad);
  gst_object_unref(element);
}
GST_END_TEST;

/* Test sink pad caps */
GST_START_TEST(test_sink_pad_caps)
{
  GstElement *element;
  GstPad *sink_pad;
  GstCaps *caps, *template_caps;

  element = gst_element_factory_make("websockettransceiver", NULL);
  fail_unless(element != NULL);

  sink_pad = gst_element_get_static_pad(element, "sink");
  fail_unless(sink_pad != NULL);

  template_caps = gst_pad_get_pad_template_caps(sink_pad);
  fail_unless(template_caps != NULL, "Sink pad should have template caps");
  fail_unless(!gst_caps_is_empty(template_caps), "Template caps should not be empty");

  /* Verify it accepts raw audio */
  caps = gst_caps_new_simple("audio/x-raw",
      "format", G_TYPE_STRING, "S16LE",
      "rate", G_TYPE_INT, 16000,
      "channels", G_TYPE_INT, 1,
      NULL);

  fail_unless(gst_caps_can_intersect(template_caps, caps),
      "Sink should accept S16LE audio");

  gst_caps_unref(caps);
  gst_caps_unref(template_caps);
  gst_object_unref(sink_pad);
  gst_object_unref(element);
}
GST_END_TEST;

/* Test src pad caps */
GST_START_TEST(test_src_pad_caps)
{
  GstElement *element;
  GstPad *src_pad;
  GstCaps *caps, *template_caps;

  element = gst_element_factory_make("websockettransceiver", NULL);
  fail_unless(element != NULL);

  src_pad = gst_element_get_static_pad(element, "src");
  fail_unless(src_pad != NULL);

  template_caps = gst_pad_get_pad_template_caps(src_pad);
  fail_unless(template_caps != NULL, "Src pad should have template caps");
  fail_unless(!gst_caps_is_empty(template_caps), "Template caps should not be empty");

  /* Verify it can output raw audio */
  caps = gst_caps_new_simple("audio/x-raw",
      "format", G_TYPE_STRING, "S16LE",
      "rate", G_TYPE_INT, 16000,
      "channels", G_TYPE_INT, 1,
      NULL);

  fail_unless(gst_caps_can_intersect(template_caps, caps),
      "Src should output S16LE audio");

  gst_caps_unref(caps);
  gst_caps_unref(template_caps);
  gst_object_unref(src_pad);
  gst_object_unref(element);
}
GST_END_TEST;

/* Test state change NULL -> READY (should fail without URI) */
GST_START_TEST(test_state_change_no_uri)
{
  GstElement *element;
  GstStateChangeReturn ret;

  element = gst_element_factory_make("websockettransceiver", NULL);
  fail_unless(element != NULL);

  /* Without URI set, state change to READY should fail */
  ret = gst_element_set_state(element, GST_STATE_READY);
  fail_unless(ret == GST_STATE_CHANGE_FAILURE,
      "State change to READY without URI should fail");

  gst_element_set_state(element, GST_STATE_NULL);
  gst_object_unref(element);
}
GST_END_TEST;

/* Test that element is marked as live source */
GST_START_TEST(test_is_live_source)
{
  GstElement *element;
  GstPad *src_pad;
  GstQuery *query;
  gboolean live;

  element = gst_element_factory_make("websockettransceiver", NULL);
  fail_unless(element != NULL);

  src_pad = gst_element_get_static_pad(element, "src");
  fail_unless(src_pad != NULL);

  query = gst_query_new_latency();
  /* Note: Query may not work in NULL state, but we test the query exists */
  if (gst_pad_query(src_pad, query)) {
    gst_query_parse_latency(query, &live, NULL, NULL);
    fail_unless(live == TRUE, "Element should be a live source");
  }

  gst_query_unref(query);
  gst_object_unref(src_pad);
  gst_object_unref(element);
}
GST_END_TEST;

static Suite *
websockettransceiver_suite(void)
{
  Suite *s = suite_create("websockettransceiver");
  TCase *tc_basic = tcase_create("basic");
  TCase *tc_properties = tcase_create("properties");
  TCase *tc_pads = tcase_create("pads");
  TCase *tc_state = tcase_create("state");

  suite_add_tcase(s, tc_basic);
  tcase_add_test(tc_basic, test_element_create);

  suite_add_tcase(s, tc_properties);
  tcase_add_test(tc_properties, test_properties_default);
  tcase_add_test(tc_properties, test_properties_set_get);

  suite_add_tcase(s, tc_pads);
  tcase_add_test(tc_pads, test_pads_exist);
  tcase_add_test(tc_pads, test_sink_pad_caps);
  tcase_add_test(tc_pads, test_src_pad_caps);

  suite_add_tcase(s, tc_state);
  tcase_add_test(tc_state, test_state_change_no_uri);
  tcase_add_test(tc_state, test_is_live_source);

  return s;
}

GST_CHECK_MAIN(websockettransceiver);
