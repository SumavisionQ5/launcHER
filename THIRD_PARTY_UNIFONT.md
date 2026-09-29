# GNU Unifont notice

The Chinese/CJK bitmap glyphs embedded in the Chinese launcHER build are
generated from GNU Unifont 18.0.01.

Project page:
https://unifoundry.com/unifont/index.html

The font is dual-licensed under the GNU GPL v2 or later with the GNU Font
Embedding Exception and the SIL Open Font License 1.1. See the official
Unifont page and license files for the complete terms:

- https://unifoundry.com/unifont/index.html
- https://unifoundry.com/LICENSE.txt
- https://unifoundry.com/OFL-1.1.txt

The build downloads the pinned 18.0.01 HEX source at build time and converts
only the BMP glyphs referenced by the launcher's UTF-8 source strings into an
embedded 16x16 monochrome bitmap table.
