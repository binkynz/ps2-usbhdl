FROM ps2dev/ps2dev:latest

# ps2dev/ps2dev is Alpine-based and ships only the cross-compilers.
# - make: PS2SDK uses GNU make
# - bash: Makefile.pref relies on bash-isms
# - gmp / mpfr / mpc: GCC 15's cc1 dynamically links against these
#   and Alpine doesn't include them by default
RUN apk add --no-cache make bash gmp mpfr4 mpc1
