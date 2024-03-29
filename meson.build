project('gnome-software', 'c',
  version : '41.1',
  license : 'GPL-2.0+',
  default_options : ['warning_level=1', 'c_std=c11'],
  meson_version : '>=0.47.0'
)

conf = configuration_data()
conf.set_quoted('VERSION', meson.project_version())
conf.set_quoted('PACKAGE_NAME', meson.project_name())
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('BUILD_TYPE', get_option('buildtype'))

build_profile = get_option('profile')
if get_option('buildtype') == 'release'
  build_profile = ''
endif

conf.set_quoted('BUILD_PROFILE', build_profile)

application_id = 'org.gnome.Software' + build_profile
conf.set_quoted('APPLICATION_ID', application_id)

# this refers to the gnome-software plugin API version
# this is not in any way related to a package or soname version
gs_plugin_api_version = '16'
conf.set_quoted('GS_PLUGIN_API_VERSION', gs_plugin_api_version)

# private subdirectory of libdir for the private shared libgnomesoftware to live in
gs_private_libdir = join_paths(get_option('prefix'), get_option('libdir'), 'gnome-software')

# install docs
install_data('README.md', install_dir : 'share/doc/gnome-software')

# get supported warning flags
test_args = [
  '-fstack-protector-strong',
  '-Waggregate-return',
  '-Warray-bounds',
  '-Wcast-align',
  '-Wclobbered',
  '-Wdeclaration-after-statement',
  '-Wempty-body',
  '-Wextra',
  '-Wformat=2',
  '-Wformat-nonliteral',
  '-Wformat-security',
  '-Wformat-signedness',
  '-Wignored-qualifiers',
  '-Wimplicit-function-declaration',
  '-Werror=implicit-function-declaration',
  '-Winit-self',
  '-Winline',
  '-Wmaybe-uninitialized',
  '-Wmissing-declarations',
  '-Wmissing-format-attribute',
  '-Wmissing-include-dirs',
  '-Wmissing-noreturn',
  '-Wmissing-parameter-type',
  '-Wmissing-prototypes',
  '-Wnested-externs',
  '-Werror=nested-externs',
  '-Wno-missing-field-initializers',
  '-Wno-strict-aliasing',
  '-Wno-suggest-attribute=format',
  '-Wno-unused-parameter',
  '-Wold-style-definition',
  '-Woverride-init',
  '-Wpacked',
  '-Wpointer-arith',
  '-Wredundant-decls',
  '-Wreturn-type',
  '-Wshadow',
  '-Wsign-compare',
  '-Wstrict-aliasing',
  '-Wstrict-prototypes',
  '-Wswitch-default',
  '-Wtype-limits',
  '-Wundef',
  '-Wuninitialized',
  '-Wunused-but-set-variable',
  '-Wwrite-strings'
]
cc = meson.get_compiler('c')
foreach arg: test_args
  if cc.has_argument(arg)
    add_project_arguments(arg, language : 'c')
  endif
endforeach

# enable full RELRO where possible
# FIXME: until https://github.com/mesonbuild/meson/issues/1140 is fixed
global_link_args = []
test_link_args = [
  '-Wl,-z,relro',
  '-Wl,-z,now',
]
foreach arg: test_link_args
  if cc.has_link_argument(arg)
    global_link_args += arg
  endif
endforeach
add_global_link_arguments(
  global_link_args,
  language: 'c'
)

# Needed for PATH_MAX and symlink()
add_project_arguments('-D_XOPEN_SOURCE=700', language : 'c')
# Needed for syscall()
add_project_arguments('-D_GNU_SOURCE', language : 'c')

conf.set('HAVE_LINUX_UNISTD_H', cc.has_header('linux/unistd.h'))

appstream = dependency('appstream',
  version : '>= 0.14.0',
  fallback : ['appstream', 'appstream_dep'],
  default_options : [
    'docs=false',
    'apidocs=false',
    'install-docs=false'
  ]
)
gdk_pixbuf = dependency('gdk-pixbuf-2.0', version : '>= 2.32.0')
libxmlb = dependency('xmlb', version : '>= 0.1.7', fallback : ['libxmlb', 'libxmlb_dep'])
gio_unix = dependency('gio-unix-2.0', version : '>= 2.56.0')
gmodule = dependency('gmodule-2.0')
gtk = dependency('gtk+-3.0', version : '>= 3.22.4')
glib = dependency('glib-2.0', version : '>= 2.55.0')
json_glib = dependency('json-glib-1.0', version : '>= 1.2.0')
libm = cc.find_library('m', required: false)
libsoup = dependency('libsoup-2.4', version : '>= 2.52.0')
libhandy = dependency('libhandy-1',
  version: '>=1.2.0',
  fallback: ['libhandy', 'libhandy_dep'],
  default_options: [
      'examples=false',
      'glade_catalog=disabled',
      'introspection=disabled',
      'static=true',
      'tests=false',
      'vapi=false',
    ]
  )

libsysprof_capture_dep = dependency('sysprof-capture-4',
  required: get_option('sysprof'),
  default_options: [
    'enable_examples=false',
    'enable_gtk=false',
    'enable_tests=false',
    'enable_tools=false',
    'libsysprof=false',
    'with_sysprofd=none',
    'help=false',
  ],
  fallback: ['sysprof', 'libsysprof_capture_dep'],
)
conf.set('HAVE_SYSPROF', libsysprof_capture_dep.found())

if get_option('mogwai')
  mogwai_schedule_client = dependency('mogwai-schedule-client-0', version : '>= 0.2.0')
  conf.set('HAVE_MOGWAI', 1)
endif

if get_option('valgrind')
  valgrind = dependency('valgrind')
  conf.set('USE_VALGRIND', 1)
else
  message('Valgrind integration disabled')
  valgrind = []
endif

gsettings_desktop_schemas = dependency('gsettings-desktop-schemas', version : '>= 3.18.0', required: get_option('gsettings_desktop_schemas'))
if gsettings_desktop_schemas.found()
  conf.set('HAVE_GSETTINGS_DESKTOP_SCHEMAS', 1)
endif

if get_option('gspell')
  gspell = dependency('gspell-1')
  conf.set('HAVE_GSPELL', 1)
endif

if get_option('polkit')
  polkit = dependency('polkit-gobject-1')
  conf.set('HAVE_POLKIT', 1)
endif

if get_option('packagekit')
  packagekit = dependency('packagekit-glib2', version : '>= 1.1.0')
  conf.set('HAVE_PACKAGEKIT', '1')
  add_project_arguments('-DI_KNOW_THE_PACKAGEKIT_GLIB2_API_IS_SUBJECT_TO_CHANGE',
                        language : 'c')
  if get_option('packagekit_autoremove')
      conf.set('GS_PACKAGEKIT_AUTOREMOVE', 'TRUE')
  else
      conf.set('GS_PACKAGEKIT_AUTOREMOVE', 'FALSE')
  endif
  if meson.get_compiler('c').has_function('pk_package_get_update_severity', prefix: '#include <packagekit-glib2/packagekit.h>', dependencies: packagekit)
      conf.set('HAVE_PK_PACKAGE_GET_UPDATE_SEVERITY', '1')
  endif
  if meson.get_compiler('c').has_function('pk_details_get_download_size', prefix: '#include <packagekit-glib2/packagekit.h>', dependencies: packagekit)
      conf.set('HAVE_PK_DETAILS_GET_DOWNLOAD_SIZE', '1')
  endif
  if meson.get_compiler('c').has_function('pk_offline_cancel_with_flags', prefix: '#include <packagekit-glib2/packagekit.h>', dependencies: packagekit) and \
     meson.get_compiler('c').has_function('pk_offline_trigger_with_flags', prefix: '#include <packagekit-glib2/packagekit.h>', dependencies: packagekit) and \
     meson.get_compiler('c').has_function('pk_offline_trigger_upgrade_with_flags', prefix: '#include <packagekit-glib2/packagekit.h>', dependencies: packagekit)
      conf.set('HAVE_PK_OFFLINE_WITH_FLAGS', '1')
  endif
endif

if get_option('eos_updater')
  ostree = dependency('ostree-1')
endif

if get_option('fwupd')
  fwupd = dependency('fwupd', version : '>= 1.0.3')
endif

if get_option('flatpak')
  flatpak = dependency('flatpak', version : '>= 1.0.4')
  ostree = dependency('ostree-1')
endif

if get_option('malcontent')
  malcontent = dependency('malcontent-0', version: '>= 0.3.0', fallback: ['malcontent', 'libmalcontent_dep'], default_options: ['ui=disabled'])
endif

if get_option('rpm_ostree')
  libdnf = dependency('libdnf')
  ostree = dependency('ostree-1')
  rpm = dependency('rpm')
  rpm_ostree = dependency('rpm-ostree-1', version : '>= 2019.3')
endif

if get_option('gudev')
  gudev = dependency('gudev-1.0')
endif

if get_option('snap')
  snap = dependency('snapd-glib', version : '>= 1.50')
endif

gnome = import('gnome')
i18n = import('i18n')

conf.set_quoted('GETTEXT_PACKAGE', meson.project_name())
conf.set_quoted('LOCALEDIR',
                join_paths(get_option('prefix'),
                           get_option('localedir')))
conf.set_quoted('DATADIR',
                join_paths(get_option('prefix'),
                           get_option('datadir')))
conf.set_quoted('LIBDIR',
                join_paths(get_option('prefix'),
                           get_option('libdir')))
conf.set_quoted('BINDIR',
                join_paths(get_option('prefix'),
                           get_option('bindir')))
conf.set_quoted('SYSCONFDIR',
                join_paths(get_option('prefix'),
                           get_option('sysconfdir')))
conf.set_quoted('LOCALSTATEDIR',
                join_paths(get_option('prefix'),
                           get_option('localstatedir')))
conf.set_quoted('LIBEXECDIR',
                join_paths(get_option('prefix'),
                           get_option('libexecdir')))
conf.set('ENABLE_EXTERNAL_APPSTREAM', get_option('external_appstream'))
configure_file(
  output : 'config.h',
  configuration : conf
)

add_project_arguments('-DI_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE',
                      language : 'c')

test_env = [
  'G_TEST_SRCDIR=@0@'.format(meson.current_source_dir()),
  'G_TEST_BUILDDIR=@0@'.format(meson.current_build_dir()),
  'GSETTINGS_SCHEMA_DIR=@0@/data/'.format(meson.build_root()),
  'GSETTINGS_BACKEND=memory',
  'MALLOC_CHECK_=2',
]

subdir('data')
subdir('lib')
subdir('plugins')
subdir('src')
if get_option('external_appstream')
  subdir('gs-install-appstream')
endif
subdir('po')
subdir('doc')

# FIXME: remove when https://github.com/mesonbuild/meson/issues/837 fixed
meson.add_install_script('meson_post_install.sh')
