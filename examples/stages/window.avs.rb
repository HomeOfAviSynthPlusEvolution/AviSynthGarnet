# This is a pipeline stage, not a standalone source or a cached library.
# ImportScript supplies AVS `last`; Ruby's last helper reads that input clip.
# Local AVS variables/last are scoped to the stage, but explicit globals and
# filter registrations are not isolated. Do not register filters in a stage.
input = last

# import_relative passes this receiver as the nested script's last, resolves
# relative to this Ruby file, and runs the native AVS stage below.
trimmed = input.import_relative('trim.avs')

# Return a clip as the final expression. Bare `last` here would still be the
# stage input, not the value of the preceding Ruby expression.
trimmed.BilinearResize(360, 240)
