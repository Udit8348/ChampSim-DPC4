# L1D Perfect Oracle
Using a debug flag at compile time we can enable this
Simulates a perfect L1D cache, to determine maximum theoretical performance ceiling
Does not dictate how a perfect prefetcher might look like since replacement, timeliness and bandwidth etc are not considered here
Rather, we are more interested in which traces have the most opportunity for performance gains
This insight can be used to guide refined prefetcher efforts
An assertion is hit if there is a cache miss (should never happen)

This is implemented as a conditional copmilation in `cache.cc`. We can conditionally enable it using the following build process:

```
# build in oracle mode
make clean && make CPPFLAGS+=-DCHAMPSIM_ORACLE_L1D
```