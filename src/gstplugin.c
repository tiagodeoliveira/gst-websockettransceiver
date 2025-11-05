#include <gst/gst.h>
#include "gstwebsockettransceiver.h"

static gboolean
plugin_init(GstPlugin *plugin)
{
  return gst_element_register(plugin, "websockettransceiver",
      GST_RANK_NONE, GST_TYPE_WEBSOCKET_TRANSCEIVER);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    websockettransceiver,
    "WebSocket audio transceiver for AI voice bots",
    plugin_init,
    "0.1.0",
    "LGPL",
    "GstWebSocket",
    "https://github.com/tiagodeoliveira/gst-websockettransceiver"
)
