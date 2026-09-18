# Called only after minirake succeeds; never mark a partial rebuild as reusable.
file(READ "${SETTINGS}" manifest)
file(SHA256 "${ARCHIVE}" archive_hash)
string(JSON manifest SET "${manifest}" library_sha256 "\"${archive_hash}\"")
file(WRITE "${MANIFEST}.tmp" "${manifest}\n")
file(RENAME "${MANIFEST}.tmp" "${MANIFEST}")
