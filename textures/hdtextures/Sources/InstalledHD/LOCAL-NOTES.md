# Local PSPRecomp integration

Source: https://github.com/SonofUgly/VCS-Texture-Pack

Revision: `93944f237e1c348c2d61645b4b859a7f4b5cd73e`.

The upstream README and all 207 PNG files are preserved. The local textures.ini
fixes 31 paths to files in existing subdirectories and omits 421 mappings whose
PNG files are absent from that revision. It contains 230 existing mappings.
Unmatched game textures use the original PSP assets. No texture artwork was
generated or modified for this integration.

The loader implements the pack's PPSSPP quick-hash keys and PNG replacement
images, with mipmaps generated at runtime. It is not a complete implementation
of every PPSSPP texture-pack option.
