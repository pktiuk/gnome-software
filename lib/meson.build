cargs = ['-DG_LOG_DOMAIN="Gs"']
cargs += ['-DLOCALPLUGINDIR=""']

libgnomesoftware_public_headers = [
  'gnome-software.h',
  'gs-app.h',
  'gs-app-collation.h',
  'gs-app-list.h',
  'gs-appstream.h',
  'gs-autocleanups.h',
  'gs-category.h',
  'gs-category-manager.h',
  'gs-desktop-data.h',
  'gs-external-appstream-utils.h',
  'gs-icon.h',
  'gs-ioprio.h',
  'gs-key-colors.h',
  'gs-metered.h',
  'gs-odrs-provider.h',
  'gs-os-release.h',
  'gs-plugin.h',
  'gs-plugin-event.h',
  'gs-plugin-job.h',
  'gs-plugin-loader.h',
  'gs-plugin-loader-sync.h',
  'gs-plugin-types.h',
  'gs-plugin-vfuncs.h',
  'gs-remote-icon.h',
  'gs-utils.h'
]

install_headers(libgnomesoftware_public_headers,
  subdir : 'gnome-software'
)

librarydeps = [
  appstream,
  gio_unix,
  glib,
  gmodule,
  gtk,
  json_glib,
  libm,
  libsoup,
  libsysprof_capture_dep,
  libxmlb,
  valgrind,
]

if get_option('mogwai')
  librarydeps += mogwai_schedule_client
endif

if get_option('polkit')
  librarydeps += polkit
endif

gs_build_ident_h = vcs_tag(
     fallback: meson.project_version(),
        input: 'gs-build-ident.h.in',
       output: 'gs-build-ident.h',
)

libgnomesoftware_enums = gnome.mkenums_simple('gs-enums',
  sources : libgnomesoftware_public_headers,
  install_header : true,
  install_dir : join_paths(get_option('includedir'), 'gnome-software'),
)

libgnomesoftware_include_directories = [
  include_directories('..'),
]

libgnomesoftware = library(
  'gnomesoftware',
  sources : [
    'gs-app.c',
    'gs-app-list.c',
    'gs-appstream.c',
    'gs-category.c',
    'gs-category-manager.c',
    'gs-debug.c',
    'gs-desktop-data.c',
    'gs-external-appstream-utils.c',
    'gs-fedora-third-party.c',
    'gs-icon.c',
    'gs-ioprio.c',
    'gs-ioprio.h',
    'gs-key-colors.c',
    'gs-metered.c',
    'gs-odrs-provider.c',
    'gs-os-release.c',
    'gs-plugin.c',
    'gs-plugin-event.c',
    'gs-plugin-job.c',
    'gs-plugin-loader.c',
    'gs-plugin-loader-sync.c',
    'gs-remote-icon.c',
    'gs-test.c',
    'gs-utils.c',
  ] + libgnomesoftware_enums + [gs_build_ident_h],
  soversion: gs_plugin_api_version,
  include_directories : libgnomesoftware_include_directories,
  dependencies : librarydeps,
  c_args : cargs,
  install: true,
  install_dir: gs_private_libdir,
)

libgnomesoftware_dep = declare_dependency(link_with : libgnomesoftware,
  sources : libgnomesoftware_enums,
  include_directories : libgnomesoftware_include_directories,
  dependencies: librarydeps,
)

pkg = import('pkgconfig')

pkg.generate(
    libgnomesoftware,
    description : 'GNOME Software is a software center for GNOME',
    filebase : 'gnome-software',
    name : 'gnome-software',
    subdirs : 'gnome-software',
    variables : [
      'gs_private_libdir=${libdir}/gnome-software',
      'plugindir=${gs_private_libdir}/plugins-' + gs_plugin_api_version,
    ],
    install_dir : join_paths(get_option('prefix'), get_option('libdir'), 'pkgconfig'),  # or it defaults to gs_private_libdir, which is wrong
)

executable(
  'gnome-software-cmd',
  sources : [
    'gs-cmd.c',
  ],
  include_directories : [
    include_directories('..'),
  ],
  dependencies : [
    appstream,
    gio_unix,
    glib,
    gmodule,
    gtk,
    json_glib,
    libgnomesoftware_dep,
    libm,
    libsoup,
    valgrind,
  ],
  c_args : cargs,
  install : true,
  install_dir : get_option('libexecdir'),
  install_rpath : gs_private_libdir,
)

if get_option('tests')
  cargs += ['-DTESTDATADIR="' + join_paths(meson.current_source_dir(), '..', 'data') + '"']
  e = executable(
    'gs-self-test',
    compiled_schemas,
    sources : [
      'gs-self-test.c'
    ],
    include_directories : [
      include_directories('..'),
    ],
    dependencies : [
      appstream,
      gio_unix,
      glib,
      gmodule,
      gtk,
      json_glib,
      libgnomesoftware_dep,
      libm,
      libsoup
    ],
    c_args : cargs
  )
  test('gs-self-test-lib', e, suite: ['lib'], env: test_env, timeout : 120)
endif

subdir('tools')
