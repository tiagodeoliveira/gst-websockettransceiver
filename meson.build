project('gst-websockettransceiver', 'c',
  version: '0.1.0',
  default_options: ['warning_level=2', 'c_std=gnu11'])

gst_req = '>= 1.20.0'
glib_req = '>= 2.62'
soup_req = '>= 3.0'

gst_dep = dependency('gstreamer-1.0', version: gst_req)
gst_base_dep = dependency('gstreamer-base-1.0', version: gst_req)
gst_audio_dep = dependency('gstreamer-audio-1.0', version: gst_req)
glib_dep = dependency('glib-2.0', version: glib_req)
soup_dep = dependency('libsoup-3.0', version: soup_req)

plugins_install_dir = get_option('libdir') / 'gstreamer-1.0'

subdir('src')
