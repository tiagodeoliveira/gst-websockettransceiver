# Test dependencies
gst_check_dep = dependency('gstreamer-check-1.0', version: gst_req, required: true)

# Get the plugin build directory for GST_PLUGIN_PATH
plugin_dir = meson.project_build_root() / 'src'

# Common test environment
test_env = environment()
test_env.set('GST_PLUGIN_PATH', plugin_dir)
# test_env.set('GST_PLUGIN_SYSTEM_PATH', '')
test_env.set('GST_STATE_IGNORE_ELEMENTS', '')
test_env.set('CK_DEFAULT_TIMEOUT', '20')

# Test dependencies
test_deps = [
  gst_dep,
  gst_base_dep,
  gst_check_dep,
  glib_dep,
]

# Basic element tests (fast, no network)
test_websockettransceiver = executable('test_websockettransceiver',
  'check/elements/websockettransceiver.c',
  dependencies: test_deps,
)

test('websockettransceiver',
  test_websockettransceiver,
  env: test_env,
  timeout: 60,
)

# Integration tests with external WebSocket server
test_websockettransceiver_integration = executable('test_websockettransceiver_integration',
  'check/elements/websockettransceiver_integration.c',
  dependencies: test_deps,
)

# Use wrapper script to start stub server before running tests
test_wrapper = find_program('run_integration_test.sh')

test('websockettransceiver_integration',
  test_wrapper,
  args: [test_websockettransceiver_integration],
  env: test_env,
  timeout: 120,
)
