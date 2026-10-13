import threading,time
from backend.collector import Collector

def test_database_failure_does_not_kill_worker_and_new_packet_recovers():
    class Store:
        archive_error=None;error=None;rejected=0
        def __init__(self):self.failed=threading.Event();self.recovered=threading.Event()
        def check_offline(self):
            if not self.failed.is_set():
                self.failed.set();raise OSError('database unavailable')
        def ingest(self,*args):self.recovered.set()
    store=Store();collector=Collector(store,{'state_topic':'state','event_topic':'event'})
    thread=threading.Thread(target=collector.run);thread.start()
    try:
        assert store.failed.wait(2)
        # Wait until the exception handler has marked the observer uncertain.
        deadline=time.monotonic()+1
        while store.archive_error is None and time.monotonic()<deadline:time.sleep(.01)
        assert thread.is_alive() and store.archive_error
        collector.queue.put(('event',b'{"v":3,"d":[1,1,10,1,0,0,100,0,0,400]}',100,False))
        assert store.recovered.wait(2)
        deadline=time.monotonic()+1
        while store.archive_error and time.monotonic()<deadline:time.sleep(.01)
        assert thread.is_alive() and store.archive_error is None
    finally:collector.stop_event.set();thread.join(2)
