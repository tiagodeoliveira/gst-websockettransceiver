plugin_sources = [
  'gstplugin.c',
  'gstwebsockettransceiver.c',
]

gstwebsockettransceiver = library('gstwebsockettransceiver',
  plugin_sources,
  c_args: ['-DPACKAGE="gst-websockettransceiver"'],
  dependencies: [gst_dep, gst_base_dep, gst_audio_dep, glib_dep, soup_dep, json_dep],
  install: true,
  install_dir: plugins_install_dir,
)
