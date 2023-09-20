<%
    OneApi=tags['$OneApi']
    x=tags['$x']
    X=x.upper()
%>

=============================
AMD HIP UR Reference Document
=============================

This document gives general guidelines of how to use UR to execute kernels on
a AMD HIP device.

Device code
===========

Unlike the NVPTX platform, AMDGPU does not use a device IR that can be JIT
compiled at runtime. Therefore, all device binaries must be precompiled for a
particular arch.

The naming of AMDGPU device code files may vary across different generations
of devices. ``.hsa`` or ``.hsaco`` are common extensions as of 2023.

HIPCC can generate device code for a particular arch using the ``--genco`` flag

.. code-block:: console

    $ hipcc --genco hello.cu --amdgpu-target=gfx906 -o hello.hsaco

UR Programs
===========

A ${x}_program_handle_t has a one to one mapping with the HIP runtime object
`hipModule_t <https://docs.amd.com/projects/HIP/en/latest/.doxygen/docBin/html/group___module.html>`__

In UR for HIP, a ${x}_program_handle_t can be created using
${x}ProgramCreateWithBinary with:

* A single device code module

A ${x}_program_handle_t is valid only for a single architecture. If a HIP
compatible binary contains device code for multiple AMDGPU architectures, it is
the user's responsibility to split these separate device images so that
${x}ProgramCreateWithBinary is only called with a device binary for a single
device arch.

If the AMDGPU module is incompatible with the device arch then ${x}ProgramBuild
will fail with the error ``hipErrorNoBinaryForGpu``.

If a program is large and contains many kernels, loading the program may have a
high overhead. This can be mitigated by splitting a program into multiple
smaller programs. In this way, an application will only pay the overhead of
loading kernels that it will likely use.

Kernels
=======

Once ${x}ProgramCreateWithBinary and ${x}ProgramBuild have succeeded, kernels
can be fetched from programs with ${x}KernelCreate. ${x}KernelCreate must be
called with the exact name of the kernel in the AMDGPU device code module. This
name will depend on the mangling used when compiling the kernel, so it is
recommended to examine the symbols in the AMDGPU device code module before
trying to extract kernels in UR code.

``llvm-objdump`` or ``readelf`` may not correctly view the symbols in an AMDGPU
device module. It may be necessary to call ``clang-offload-bundler`` first in
order to extract the ``ELF`` file that can be passed to ``readelf``.

.. code-block:: console

    $ clang-offload-bundler --unbundle --input=hello.hsaco --output=hello.o \
        --targets=hipv4-amdgcn-amd-amdhsa--gfx906 --type=o
    $ readelf hello.o -s | grep mykernel
    _Z13mykernelv

At present it is not possible to query the names of the kernels in a UR program
for HIP, so it is necessary to know the (mangled or otherwise) names of kernels
in advance or by some other means.

UR kernels can be dispatched with ${x}EnqueueKernelLaunch. The argument
``pGlobalWorkOffset`` can only be used if the kernels have been instrumented to
take the extra global offset argument. Use of the global offset is not
recommended for non SYCL compiler toolchains. This parameter can be ignored if
the user does not wish to use the global offset.

Other Notes
===========

- In kernel ``printf`` may not work for certain ROCm versions.

Contributors
------------

* Hugh Delaney `hugh.delaney@codeplay.com <hugh.delaney@codeplay.com>`_

