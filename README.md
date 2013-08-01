BUILD STATUS
------------
Current build status:
[![Build Status](https://travis-ci.org/Islandora/islandora_bookmark.png?branch=7.x)](https://travis-ci.org/Islandora/islandora_bookmark)

CI Server:
http://jenkins.discoverygarden.ca

ISLANDORA IMPORTER
==================

CONTENTS OF THIS FILE
---------------------

 * summary
 * requirements
 * installation


SUMMARY
-------

Islandora Importer

Made primarily for ingesting objects described by MODS and DC.

A number of importer modules have been included, which should be described in
greater detail where they reside (in the "modules" directory):
- RIS*
- EndNote XML*
- Pubmed*
- Digital Object Identifiers (DOI)*
- Zip file

(* Currently, only creates "citation" documents, as used in the
islandora_scholar module)

REQUIREMENTS
------------

The following Drupal modules are required:
 * islandora

INSTALLATION
------------

Enable the module.
