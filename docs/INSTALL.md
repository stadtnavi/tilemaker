# Installing tilemaker

### macOS

Install all dependencies with Homebrew:

    brew install protobuf boost lua51 shapelib

Then:

    make
    sudo make install

### Ubuntu

Start with:

    sudo apt-get install build-essential liblua5.1-0 liblua5.1-0-dev libprotobuf-dev libsqlite3-dev protobuf-compiler shapelib libshp-dev
    sudo apt-get install libboost-all-dev

Boost should be version 1.66 or later. If that's not available on your system, then you can install a more recent 
version from [this PPA](https://launchpad.net/~mhier/+archive/ubuntu/libboost-latest).

Once you've installed those, then `cd` back to your Tilemaker directory and simply:

    make
    sudo make install

If it fails, check that the LIB and INC lines in the Makefile correspond with your system, then try again.

### Fedora

Start with:

    dnf install lua-devel luajit-devel sqlite-devel protobuf-devel protobuf-compiler shapelib-devel

then build either with lua:

    make LUA_CFLAGS="$(pkg-config --cflags lua)" LUA_LIBS="$(pkg-config --libs lua)"
    make install

or with luajit:

    make LUA_CFLAGS="$(pkg-config --cflags luajit)" LUA_LIBS="$(pkg-config --libs luajit)"
    make install

### Using cmake

You can optionally use cmake to build:

    mkdir build
    cd build
    cmake ..
    make
    sudo make install

### Docker

**The Dockerfile is not formally supported by project maintainers and you are encouraged to send pull requests to fix any issues you encounter.**

Build from project root directory with:

    docker build . -t tilemaker

The docker container can be run like this:

    docker run -v /Users/Local/Downloads/:/srv -i -t --rm tilemaker /srv/germany-latest.osm.pbf --output=/srv/germany.mbtiles

Keep in mind to map the volume your .osm.pbf files are in to a path within your docker container, as seen in the example above. 
