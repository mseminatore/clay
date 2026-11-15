#ifndef CLAY_SEMVER_H
#define CLAY_SEMVER_H

/* npm-style semver ranges, the rules of stdlib/semver's `satisfies`:
 * "^1.2", "~1.2.3", ">=1 <2", "1.2.x", "1.0.0 - 2.1", alternatives with "||".
 * A prerelease version only satisfies a range that names a prerelease of the
 * same major.minor.patch.
 *
 * Returns 1 if version satisfies range, 0 if not, -1 if either is invalid. */
int semver_satisfies(const char *version, const char *range);

#endif /* CLAY_SEMVER_H */
