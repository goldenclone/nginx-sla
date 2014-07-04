# nginx-sla

Module [nginx](http://nginx.org/ru/) for implementing the collection of augmented statistics based on HTTP-codes and upstreams response time for further transmission to monitoring tools such as [zabbix](http://www.zabbix.com/).

Module answers the following questions:

* What is the average response time of an upstream (backend)?
* How many answers with an HTTP-status were there - 200, 404?
* How many answers were there within different HTTP-status classes - 2xx, 5xx?
* For how many queries did upstream answer for less than 300ms? For how many queries did upstream answer in the time boundaries within 300ms to 500ms?
* How much time did it took for upstream to handle 90%, 99% of queries?

## Assembling

For assembling purposes nginx must be configured with an additional parameter:

```
./configure --add-module=/path/to/nginx-sla
```

## Configuration

```
syntax:  sla_pool name [timings=time:time:...:time]
                       [http=status:status:...:status]
                       [avg_window=number] [min_timing=number] [default];
default: timings=300:500:2000,
         http=200:301:302:304:400:401:403:404:499:500:502:503:504,
         avg_window=1600,
         min_timing=0
context: http
```

It defines a named pool for statistics collecting. At least one pool must be specified.

* `name` - pool name that is used during statistics output and in `sla_pass` directive;
* `timings` - time lags in ms, that are used for counting a "hit" of request time;
* `http` - traceable HTTP-statuses;
* `avg_window` - window size for calculating the moving average response time;
* `min_timing` - time in ms, below which the upstreams response times aren't taken into an account;
* `default` - defines a default pool - this pool accumulates all the queries for which `sla_pass` directive doesn't clearly specify another pool.

It is recommended to choose window size for calculating the moving average response time based on the average number of dynamic queries per second multiplied by the length of data collection time.

```
syntax:  sla_alias alias name;
default: -
context: http
```

It allows defining an alias for an upstream name. It can be used for combining several upstreams under a single name or for defining usual names instead of IP addresses.

```
syntax:  sla_pass name
default: -
context: http, server, location
```

Specifies the name of the pool to which statistics must be collected. If the value is `off`, statistics collection is disabled (including collection to default pool).

```
syntax:  sla_status
default: -
context: server, location
```

Handler for statistics output.

```
syntax:  sla_purge
default: -
context: server, location
```

Handler for statistics counters' nulling.

## Sample configuration

```
sla_pool main timings=100:200:300:500:1000:2000 http=200:404 default;

sla_alias 192.168.1.1:80 frontends;
sla_alias 192.168.1.2:80 frontends;

server {
    listen [::1]:80;
    location / {
        sla_pass main;
        ...
    }
    location /sla_status {
        sla_status;
        sla_pass off;
        allow ::1;
        deny all;
    }
    location /sla_purge {
        sla_purge;
        sla_pass off;
        allow ::1;
        deny all;
    }
}
```

## Restrictions

Restrictions can be altered at compiling phase by specifying relevant preprocessor directives:

* `NGX_HTTP_SLA_MAX_NAME_LEN` - maximum length of upstream name (256 bytes by default);
* `NGX_HTTP_SLA_MAX_HTTP_LEN` - maximum number of traceable HTTP statuses (32 by default);
* `NGX_HTTP_SLA_MAX_TIMINGS_LEN` - maximum number of traceable timings (32 by default);
* `NGX_HTTP_SLA_MAX_COUNTERS_LEN` - maximum number of counters (upstreams) in the pool (16 by default).

## Statistics content

```
main.all.http = 1024
main.all.http_200 = 914
...
main.all.http_xxx = 2048
main.all.http_2xx = 914
...
main.all.time.avg = 125
main.all.time.avg.mov = 124
main.all.300 = 233
main.all.300.agg = 233
main.all.500 = 33
main.all.500.agg = 266
main.all.2000 = 40
main.all.2000.agg = 306
main.all.inf = 0
main.all.inf.agg = 306
main.all.25% = 124
main.all.75% = 126
main.all.90% = 126
main.all.99% = 130
...
main.<upstream>.http_200 = 270
...
```

* Here, the first value is a statistics pool's name;
* The second value is an upstream's name or `all` for all upstreams in the pool (including local statics);
* The third value - name of the statistics key:
  * `http` - number of processed answers with known HTTP statuses;
  * `http_200` - number of answers with HTTP-status 200 (may be `http_301`, `http_404`, `http_500` etc.);
  * `http_xxx` - number of processed answers within HTTP-status groups (in fact, the number of all processed answers);
  * `http_2xx` - number of answers in the group with HTTP-status 2xx (altogether 5 groups compliant to `1xx`, `2xx` ... `5xx`);
  * `time` - time characteristic for answers;
  * `500` - number of upstream answers within time interval of 300-500 ms;
  * `90%` - response time in ms for 90% of queries (percentile, can possess the value of `25%`, `50%`, `75%`, `90%`, `95%`, `98%`, `99%`);
  * `inf` - alias for an "infinite" time lag;
* The fourth and the fifth values - type of statistics:
  * `avg` - average;
  * `mov` - moving (average);
  * `agg` - aggregated statistics for all intervals up to the current. So, for example, 500.agg incudes all the queries that were executed between 0 and 500 ms;

## Algorithms used

EWSA ("Exponentially Weighted Stochastic Approximation") algorithm is used for percentile calculation - for details see "[Incremental Quantile Estimation for Massive Tracking](http://stat.bell-labs.com/cm/ms/departments/sia/doc/KDD2000.pdf)", Fei Chen, Diane Lambert, и Jose C. Pinheiro (2000).

Algorithm parameters can be altered at compiling phase by specifying relevant preprocessor directives:

* `NGX_HTTP_SLA_QUANTILE_M` - size of FIFO buffer for data update (100 by default);
* `NGX_HTTP_SLA_QUANTILE_W` - weighting coefficient of computed fractiles update (0.01 by default).

It makes sense to carefully read algorithm's description before changing these parameters.
