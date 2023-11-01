project('constellation_asio_sd', 'cpp',
    default_options: ['warning_level=everything'])

asio_dep = dependency('asio')

asio_test = library('asio_test',
    sources: ['BroadcastRecv.cpp', 'BroadcastSend.cpp'],
    dependencies: [asio_dep],
)

executable('broadcast_recv',
    sources: ['broadcast_recv.cpp'],
    dependencies: [asio_dep],
    link_with: [asio_test],
)

executable('broadcast_send',
    sources: ['broadcast_send.cpp'],
    dependencies: [asio_dep],
    link_with: [asio_test],
)
