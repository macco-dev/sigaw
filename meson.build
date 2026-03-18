project(
  'sigaw',
  ['c', 'cpp'],
  version: '0.1.0',
  license: 'MIT',
  default_options: [
    'c_std=c17',
    'cpp_std=c++20',
    'warning_level=2',
    'b_ndebug=if-release',
  ],
)

prefix_dir = get_option('prefix')
libdir_abs = get_option('libdir')
if not libdir_abs.startswith('/')
  libdir_abs = prefix_dir / libdir_abs
endif

bindir_abs = get_option('bindir')
if not bindir_abs.startswith('/')
  bindir_abs = prefix_dir / bindir_abs
endif

datadir_abs = get_option('datadir')
if not datadir_abs.startswith('/')
  datadir_abs = prefix_dir / datadir_abs
endif

# --------------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------------

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

# Vulkan (headers + loader)
vulkan_dep = dependency('vulkan', required: true)
gl_dep = dependency('gl', required: true)
egl_dep = dependency('egl', required: true)
x11_dep = dependency('x11', required: true)

# POSIX threads
thread_dep = dependency('threads', required: true)

# POSIX realtime (for shm_open)
rt_dep = cc.find_library('rt', required: true)
dl_dep = cc.find_library('dl', required: true)

# nlohmann/json (daemon only)
# Try system install first, fall back to subproject
json_dep = dependency('nlohmann_json', required: false)
if not json_dep.found()
  json_dep = dependency('nlohmann_json',
    fallback: ['nlohmann_json', 'nlohmann_json_dep'],
    required: true
  )
endif

# libcurl (daemon, required for OAuth2 token exchange)
curl_dep = dependency('libcurl', required: true)

# Layer text/image rendering
freetype_dep = dependency('freetype2', required: true)
png_dep = dependency('libpng', required: true)
gtk_dep = dependency('gtk+-3.0', required: true)
ayatana_appindicator_dep = dependency('ayatana-appindicator3-0.1', required: false)
use_ayatana_appindicator = ayatana_appindicator_dep.found()
appindicator_dep = ayatana_appindicator_dep
if not appindicator_dep.found()
  appindicator_dep = dependency('appindicator3-0.1', required: true)
endif
glslang_validator = find_program('glslangValidator', required: true)

overlay_quad_vert_spv = custom_target(
  'overlay-quad-vert-spv',
  input: 'src/layer/shaders/overlay_quad.vert',
  output: 'overlay_quad.vert.spv',
  command: [glslang_validator, '-V', '-S', 'vert', '-o', '@OUTPUT@', '@INPUT@'],
  build_by_default: true,
  install: true,
  install_dir: get_option('datadir') / 'sigaw' / 'shaders',
)

overlay_quad_frag_spv = custom_target(
  'overlay-quad-frag-spv',
  input: 'src/layer/shaders/overlay_quad.frag',
  output: 'overlay_quad.frag.spv',
  command: [glslang_validator, '-V', '-S', 'frag', '-o', '@OUTPUT@', '@INPUT@'],
  build_by_default: true,
  install: true,
  install_dir: get_option('datadir') / 'sigaw' / 'shaders',
)

# --------------------------------------------------------------------------
# Vulkan Layer (libSigaw.so)
# --------------------------------------------------------------------------

layer_sources = [
  'src/layer/layer_entry.c',
  'src/layer/overlay_renderer.cpp',
]

overlay_cpp_args = [
  '-DSIGAW_BUILD_DIR="' + meson.current_build_dir() + '"',
  '-DSIGAW_DATA_DIR="' + (datadir_abs / 'sigaw') + '"',
  '-DSIGAW_SOURCE_DIR="' + meson.current_source_dir() + '"',
]

layer_cpp_args = [
  '-fPIC',
  '-fvisibility=hidden',
] + overlay_cpp_args

layer_lib = shared_library(
  'Sigaw',
  layer_sources,
  include_directories: include_directories('src'),
  dependencies: [rt_dep, thread_dep, freetype_dep, png_dep],
  install: true,
  # Layer .so must be position-independent
  c_args: ['-fPIC', '-fvisibility=hidden'],
  cpp_args: layer_cpp_args,
  # Export only the Vulkan layer entry points
  link_args: [
    '-Wl,--version-script,' + meson.current_source_dir() / 'layer.map',
  ],
)

layer_gl_cpp_args = [
  '-fPIC',
  '-fvisibility=hidden',
  '-DSIGAW_DATA_DIR="' + (datadir_abs / 'sigaw') + '"',
  '-DSIGAW_SOURCE_DIR="' + meson.current_source_dir() + '"',
]

layer_gl_lib = shared_library(
  'SigawGL',
  [
    'src/layer/overlay_gl.cpp',
    'src/layer/overlay_renderer.cpp',
  ],
  include_directories: include_directories('src'),
  dependencies: [
    vulkan_dep,
    gl_dep,
    egl_dep,
    x11_dep,
    dl_dep,
    rt_dep,
    thread_dep,
    freetype_dep,
    png_dep,
  ],
  install: true,
  cpp_args: layer_gl_cpp_args,
  link_args: [
    '-Wl,--version-script,' + meson.current_source_dir() / 'layer-gl.map',
  ],
)

layer_manifest_conf = configuration_data()
layer_manifest_conf.set('layer_library_path', libdir_abs / 'libSigaw.so')
layer_manifest = configure_file(
  input: 'layer-manifest/VK_LAYER_MACCO_sigaw.x86_64.json.in',
  output: 'VK_LAYER_MACCO_sigaw.x86_64.json',
  configuration: layer_manifest_conf,
)

install_data(
  layer_manifest,
  install_dir: get_option('datadir') / 'vulkan' / 'implicit_layer.d',
)

service_conf = configuration_data()
service_conf.set('sigaw_daemon_path', bindir_abs / 'sigaw-daemon')
sigaw_service = configure_file(
  input: 'contrib/sigaw-daemon.service.in',
  output: 'sigaw-daemon.service',
  configuration: service_conf,
)

install_data(
  sigaw_service,
  install_dir: get_option('datadir') / 'systemd' / 'user',
)

install_data(
  'sigaw.conf.example',
  install_dir: get_option('datadir') / 'doc' / 'sigaw',
)

install_data(
  [
    'assets/fonts/NotoSans-Medium.ttf',
    'assets/fonts/NotoSans-Bold.ttf',
    'assets/fonts/NotoSans-LICENSE',
  ],
  install_dir: get_option('datadir') / 'sigaw' / 'fonts',
)

install_data(
  'assets/sigaw-logo.svg',
  install_dir: get_option('datadir') / 'sigaw' / 'icons',
)

# --------------------------------------------------------------------------
# Daemon (sigaw-daemon)
# --------------------------------------------------------------------------

daemon_sources = [
  'src/daemon/main.cpp',
  'src/daemon/tray_actions.cpp',
  'src/daemon/tray_icon.cpp',
  'src/layer/overlay_renderer.cpp',
]

daemon_deps = [
  vulkan_dep,
  rt_dep,
  thread_dep,
  json_dep,
  curl_dep,
  freetype_dep,
  png_dep,
  gtk_dep,
  appindicator_dep,
]

daemon_bin = executable(
  'sigaw-daemon',
  daemon_sources,
  include_directories: include_directories('src'),
  dependencies: daemon_deps,
  cpp_args: overlay_cpp_args + [
    '-DSIGAW_USE_AYATANA_APPINDICATOR=' + (use_ayatana_appindicator ? '1' : '0'),
  ],
  install: true,
)

# --------------------------------------------------------------------------
# Control CLI (sigaw-ctl)
# --------------------------------------------------------------------------

ctl_bin = executable(
  'sigaw-ctl',
  'src/ctl/main.cpp',
  include_directories: include_directories('src'),
  dependencies: [rt_dep],
  install: true,
)

sigaw_run_bin = executable(
  'sigaw-run',
  [
    'src/run/main.cpp',
    'src/run/launcher_env.cpp',
  ],
  include_directories: include_directories('src'),
  cpp_args: [
    '-DSIGAW_GL_PRELOAD_PATH="' + (libdir_abs / 'libSigawGL.so') + '"',
  ],
  install: true,
)

screenshot_bin = executable(
  'sigaw-render-screenshots',
  [
    'src/tools/render_screenshots.cpp',
    'src/layer/overlay_renderer.cpp',
  ],
  include_directories: include_directories('src'),
  dependencies: [vulkan_dep, rt_dep, thread_dep, freetype_dep, png_dep],
  cpp_args: overlay_cpp_args,
)

run_target(
  'render-readme-screenshots',
  command: [
    screenshot_bin,
    '--output-dir',
    meson.current_source_dir() / 'assets' / 'screenshots',
  ],
)

test(
  'sigaw-daemon-help',
  daemon_bin,
  args: ['--help'],
)

test(
  'sigaw-daemon-init-config',
  daemon_bin,
  args: ['--init-config'],
  env: ['XDG_CONFIG_HOME=' + (meson.current_build_dir() / 'test-config')],
)

test(
  'sigaw-ctl-help',
  ctl_bin,
  args: ['help'],
)

test(
  'sigaw-run-help',
  sigaw_run_bin,
  args: ['--help'],
)

test(
  'sigaw-run-missing-command',
  sigaw_run_bin,
  should_fail: true,
)

test(
  'sigaw-render-screenshots',
  screenshot_bin,
  args: ['--output-dir', meson.current_build_dir() / 'test-screenshots'],
)

json_utils_test = executable(
  'sigaw-json-utils-test',
  'tests/json_utils_test.cpp',
  include_directories: include_directories('src'),
  dependencies: [json_dep],
)

test(
  'sigaw-json-utils',
  json_utils_test,
)

overlay_layout_test = executable(
  'sigaw-overlay-layout-test',
  'tests/overlay_layout_test.cpp',
  include_directories: include_directories('src'),
)

test(
  'sigaw-overlay-layout',
  overlay_layout_test,
)

overlay_visibility_test = executable(
  'sigaw-overlay-visibility-test',
  'tests/overlay_visibility_test.cpp',
  include_directories: include_directories('src'),
)

test(
  'sigaw-overlay-visibility',
  overlay_visibility_test,
)

overlay_animation_test = executable(
  'sigaw-overlay-animation-test',
  'tests/overlay_animation_test.cpp',
  include_directories: include_directories('src'),
)

test(
  'sigaw-overlay-animation',
  overlay_animation_test,
)

overlay_frame_metadata_test = executable(
  'sigaw-overlay-frame-metadata-test',
  'tests/overlay_frame_metadata_test.cpp',
  include_directories: include_directories('src'),
)

test(
  'sigaw-overlay-frame-metadata',
  overlay_frame_metadata_test,
)

overlay_panel_test = executable(
  'sigaw-overlay-panel-test',
  [
    'tests/overlay_panel_test.cpp',
    'src/layer/overlay_renderer.cpp',
  ],
  include_directories: include_directories('src'),
  dependencies: [vulkan_dep, rt_dep, thread_dep, freetype_dep, png_dep],
  cpp_args: layer_cpp_args,
)

test(
  'sigaw-overlay-panel',
  overlay_panel_test,
)

config_watcher_test = executable(
  'sigaw-config-watcher-test',
  'tests/config_watcher_test.cpp',
  include_directories: include_directories('src'),
)

test(
  'sigaw-config-watcher',
  config_watcher_test,
)

config_parse_test = executable(
  'sigaw-config-parse-test',
  'tests/config_parse_test.cpp',
  include_directories: include_directories('src'),
)

test(
  'sigaw-config-parse',
  config_parse_test,
)

shm_reader_test = executable(
  'sigaw-shm-reader-test',
  'tests/shm_reader_test.cpp',
  include_directories: include_directories('src'),
  dependencies: [rt_dep],
)

test(
  'sigaw-shm-reader',
  shm_reader_test,
)

swapchain_bookkeeping_test = executable(
  'sigaw-swapchain-bookkeeping-test',
  'tests/swapchain_bookkeeping_test.cpp',
  include_directories: include_directories('src'),
  dependencies: [vulkan_dep],
)

test(
  'sigaw-swapchain-bookkeeping',
  swapchain_bookkeeping_test,
)

daemon_voice_runtime_test = executable(
  'sigaw-daemon-voice-runtime-test',
  'tests/daemon_voice_runtime_test.cpp',
  include_directories: include_directories('src'),
  dependencies: [json_dep, curl_dep],
)

test(
  'sigaw-daemon-voice-runtime',
  daemon_voice_runtime_test,
)

tray_actions_test = executable(
  'sigaw-tray-actions-test',
  [
    'tests/tray_actions_test.cpp',
    'src/daemon/tray_actions.cpp',
  ],
  include_directories: include_directories('src'),
)

test(
  'sigaw-tray-actions',
  tray_actions_test,
)

launcher_env_test = executable(
  'sigaw-launcher-env-test',
  [
    'tests/launcher_env_test.cpp',
    'src/run/launcher_env.cpp',
  ],
  include_directories: include_directories('src'),
)

test(
  'sigaw-launcher-env',
  launcher_env_test,
)

layer_runtime_test = executable(
  'sigaw-layer-runtime-test',
  'tests/layer_runtime_test.cpp',
  include_directories: include_directories('src'),
  dependencies: [vulkan_dep],
)

test(
  'sigaw-layer-runtime',
  layer_runtime_test,
)

overlay_runtime_prepare_test = executable(
  'sigaw-overlay-runtime-prepare-test',
  [
    'tests/overlay_runtime_prepare_test.cpp',
    'src/layer/overlay_renderer.cpp',
  ],
  include_directories: include_directories('src'),
  dependencies: [vulkan_dep, rt_dep, thread_dep, freetype_dep, png_dep],
  cpp_args: layer_cpp_args,
)

test(
  'sigaw-overlay-runtime-prepare',
  overlay_runtime_prepare_test,
)

sh_prog = find_program('sh', required: true)

test(
  'sigaw-layer-artifacts',
  sh_prog,
  args: [meson.current_source_dir() / 'tests' / 'layer_artifact_test.sh', layer_lib.full_path()],
)

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------

summary({
  'Vulkan layer': true,
  'OpenGL preload': true,
  'Daemon': true,
  'CLI tool': true,
  'Launcher': true,
  'Tray': true,
  'libcurl (OAuth)': curl_dep.found(),
  'nlohmann/json': json_dep.found(),
  'FreeType': freetype_dep.found(),
  'libpng': png_dep.found(),
  'AppIndicator backend': use_ayatana_appindicator ? 'Ayatana AppIndicator' : 'AppIndicator',
}, section: 'Components')
