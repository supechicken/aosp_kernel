.. raw:: latex

	\renewcommand\thesection*
	\renewcommand\thesubsection*

.. _process_index:

=============================================
Working with the kernel development community
=============================================

So you want to be a Linux kernel developer?  Welcome!  While there is a lot
to be learned about the kernel in a technical sense, it is also important
to learn about how our community works.  Reading these documents will make
it much easier for you to get your changes merged with a minimum of
trouble.

An introduction to how kernel development works
-----------------------------------------------

Read these documents first: an understanding of the material here will ease
your entry into the kernel community.

.. toctree::
   :maxdepth: 1

   howto
   development-process
   submitting-patches
   submit-checklist

Tools and technical guides for kernel developers
------------------------------------------------

This is a collection of material that kernel developers should be familiar
with.

.. toctree::
   :maxdepth: 1

   changes
   programming-language
   coding-style
   maintainer-pgp-guide
   email-clients
   applying-patches
   backporting
   adding-syscalls
   volatile-considered-harmful
   botching-up-ioctls

Policy guides and developer statements
--------------------------------------

These are the rules that we try to live by in the kernel community (and
beyond).

.. toctree::
   :maxdepth: 1

   license-rules
   code-of-conduct
   code-of-conduct-interpretation
   contribution-maturity-model
   kernel-enforcement-statement
   kernel-driver-statement
   stable-api-nonsense
   stable-kernel-rules
   management-style
   researcher-guidelines
<<<<<<< TARGET BRANCH (98ce85c5ffd4f7cb74248d35c419777fcd87de89 Merge 9f722cb7304f ("Merge tag 'alpha-for-v7.0-tag' of git:/)
   conclave
||||||| BASE          (9448598b22c50c8a5bb77a9103e2d49f134c9578 Linux 6.19-rc2)
=======
   generated-content
   coding-assistants
>>>>>>> SOURCE BRANCH (6cc45ee5df3bd89c8fa62aa2be91f747938686a1 docs: kdoc: some fixes to kernel-doc comments)

Dealing with bugs
-----------------

Bugs are a fact of life; it is important that we handle them properly. The
documents below provide general advice about debugging and describe our
policies around the handling of a couple of special classes of bugs:
regressions and security problems.

.. toctree::
   :maxdepth: 1

   debugging/index
   handling-regressions
   security-bugs
   cve
   embargoed-hardware-issues

Maintainer information
----------------------

How to find the people who will accept your patches.

.. toctree::
   :maxdepth: 1

   maintainer-handbooks
   maintainers

Other material
--------------

Here are some other guides to the community that are of interest to most
developers:

.. toctree::
   :maxdepth: 1

   kernel-docs
   deprecated

.. only::  subproject and html

   Indices
   =======

   * :ref:`genindex`
