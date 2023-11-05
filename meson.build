project('constellation_asio_sd', 'cpp',
  default_options: ['warning_level=3', 'cpp_std=c++20'],
  license: 'EUPL-1.2',
)

asio_dep = dependency('asio')
magic_enum_dep = dependency('magic_enum')

constellation_inc = include_directories('.')

subdir('CHIRP')
