Parsing and Serialization Utilities
===================================

``include/Utils/Formats/`` and ``lib/Utils/Formats/`` provide lightweight data
format support for configuration, interchange, and debugging output.

**Main components**:

- ``SExpr`` for S-expression parsing.
- ``cJSON`` and ``json11`` for JSON handling.
- ``pcomb/`` for parser-combinator based parsers.

These helpers show up throughout Lotus in spec loaders, report generation, and
small domain-specific parsers.

See also :doc:`utilities`.
