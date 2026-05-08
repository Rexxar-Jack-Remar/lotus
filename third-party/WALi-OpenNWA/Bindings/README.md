This directory has a *beta* SWIG-generated interface for portions of WALi.
I've only used it under Python 2.

THERE ARE PROBLEMS WITH THIS! THEY MAY CHANGE!


# Using

Note that there are two modules you'll need: the C Extension module
`_wali.so` and the Python wrapper `wali.py`.

Just `import wali` and hack away. Hopefully what you need will be
available. You should, I think, be able to write semiring types in
Python by extending `PySemElem` but I haven't tried this, just saw the
code sitting there, looking useful.

I'm a bit worried that some ref-counting stuff isn't in the best
shape, so hopefully you won't run into leaks or worse.




# Building

Change to this directory and run

    python setup.py ...

where the arguments are typical arguments to distutils. For example,
`install` should install it, or `build_ext` will build the extension
but not install it to `site-packages`.
