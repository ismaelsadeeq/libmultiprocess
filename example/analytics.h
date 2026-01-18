#ifndef EXAMPLE_ANALYTICS_H
#define EXAMPLE_ANALYTICS_H

#include <analytics_snapshot.h>

class Analytics {
public:
    virtual ~Analytics() = default;
    virtual AnalyticsSnapShot getAnalytics() = 0;
};
#endif // EXAMPLE_ANALYTICS_H
