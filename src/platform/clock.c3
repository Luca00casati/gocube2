// A steady clock in seconds (from an arbitrary start): for timeouts and rates.
// And fine sleeping: Windows wakes sleeping threads on its timer tick, 15.6
// ms by default, so sleeping 1 ms takes up to 16 ms; fine_sleep(true) asks
// for a 1 ms tick (per process).
module gocube::clock @link("winmm");
import gocube::win;

long frequency @private;

fn void init_frequency() @init @private => win::query_performance_frequency(&frequency);

fn double now()
{
	long t;
	win::query_performance_counter(&t);
	return (double)t / (double)frequency;
}

// Microseconds on the same clock.
fn long now_us()
{
	long t;
	win::query_performance_counter(&t);
	return (long)((int128)t * 1000000 / frequency);
}

extern fn uint time_begin_period(uint ms) @cname("timeBeginPeriod");
extern fn uint time_end_period(uint ms) @cname("timeEndPeriod");

fn void fine_sleep(bool on) => on ? (void)time_begin_period(1) : (void)time_end_period(1);

fn void sleep_ms(uint ms) => win::sleep(ms);

module gocube::clock @link("winmm");
