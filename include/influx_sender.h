#pragma once

#include <stddef.h>

struct measure;

int influx_post_measure(const struct measure *m);
int influx_post_measures(const struct measure *m, size_t count);
