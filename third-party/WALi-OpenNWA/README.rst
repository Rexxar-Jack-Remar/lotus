WALi and OpenNWA
================

Prerequsites:

* Boost  (old versions are OK; we actually variously use 1.33.1 and 1.42)
* CMake
* G++ or MSVC

Optional prerequsites for Doxygen documentation:

* Doxygen  (we use 1.7.1) 

Optional prerequsites for Latex documentation:

* Latex  (we use TexLive 2010, but it has also built with MikTex)
* `pygments <http://pygments.org/>`
* `dot2tex <http://www.fauskes.net/code/dot2tex/>`  (optional even if you are
  building the documentation; we have 2.8.7)


Building the Library
--------------------

This repository is built with CMake::

  cmake -S . -B build
  cmake --build build

By default this builds the ``wali`` library and the OpenNWA command-line
programs. Optional components can be enabled with CMake options::

  cmake -S . -B build -DWALI_BUILD_ADDONS=ON -DWALI_BUILD_EXAMPLES=ON

Useful options:

* ``WALI_BUILD_SHARED``: build shared libraries, enabled by default
* ``WALI_BUILD_TOOLS``: build OpenNWA command-line programs, enabled by default
* ``WALI_BUILD_ADDONS``: build optional add-on libraries, disabled by default.
  The bundled BuDDy library is built directly; ``walidomains`` and ``xfa`` are
  enabled only when a local ``glog`` installation is available.
* ``WALI_BUILD_EXAMPLES``: build the Reach examples, disabled by default
* ``WALI_CHECKING``: one of ``slow``, ``fast``, or ``none``; defaults to
  ``fast``

There is also a Visual Studio 2005 project, though the NWA unit tests aren't
hooked up for this at all.


WALi and OpenNWA are built to a single library. Add-ons are built as separate
libraries when their option and dependencies are available.


Building Documentation
----------------------

To get Doxygen output, run ``doxygen Doxyfile`` from the top-level directory.
By default, it will generate HTML output to ``Doc/doxygen/html`` and Latex
output to ``Doc/doxygen/latex``. (Run ``make`` in the latter directory to
build the Latex to a PDF.)

To build the NWA documentation, change to ``Doc/opennwa/`` and run
``pdflatex`` on ``opennwa.tex``, passing
``-shell-escape``. (``-shell-escape`` is required by the pygmentize package,
used for source-highlighting the example code at the beginning.) If you would
like to rebuild the figures (though this is not necessary if you do not
change them), change to Doc/opennwa/Figures and run ``make``. Note that a
couple of the figures used in the official version of the NWA manual have
been modified by hand, and regenerating them will cause those changes to be
lost.


NWA documentation
-----------------

The NWA documentation is a little bit... strange. It doesn't quite know what it
wants to be. It's way too long to just sit down and read, but is probably less
convenient to use as a reference than, say, Doxygen comments would be. (The
exception is the function tables at the end of the document.) But... we also
have Doxygen comments, and they aren't usually the same. So at some point in
the future, we need to unify the comments. (In virtually all cases, the Latex
documentation is far more detailed.)

My suggestion is to look over sections 1 ("Library Overview") and 3 ("The NWA
class"), and then refer to the more detailed sections as appropriate.
