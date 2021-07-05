proftpd-mod_dbacl
=================

Status
------
[![GitHub Actions CI Status](https://github.com/Castaglia/proftpd-mod_dbacl/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/Castaglia/proftpd-mod_dbacl/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-GPL-brightgreen.svg)](https://img.shields.io/badge/license-GPL-brightgreen.svg)

Synopsis
--------
The `mod_dbacl` module for ProFTPD uses the [`mod_sql`](http://www.proftpd.org/docs/contrib/mod_sql.html) module for SQL table access; the module uses SQL
tables for reading ACLs for files/directories.

For further module documentation, see [mod_dbacl.html](https://htmlpreview.github.io/?https://github.com/Castaglia/proftpd-mod_dbacl/blob/master/mod_dbacl.html).

Future Features
---------------
* Integrate into core engine's `dir_hide_file()` check, so that files can be
hidden via `mod_dbacl` configuration, rather than simply denying the FTP
commands which might operate directly on the file (_e.g._ if `mod_dbacl`
config would deny access to a file, do not list that file in a directory
listing of an allowed directory).
