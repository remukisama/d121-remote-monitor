import json
import threading
import time
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from backend.api import create_app
from backend.collector import Collector
from backend.mysql_store import Connection


def test_failed_persistence_is_counted_and_never_acknowledged():
    class Store:
        archive_error = None
        rejected = 0
        def ingest(self, *args): raise ValueError('database write failed')
        def check_offline(self): pass
    class Client:
        def publish(self, *args, **kwargs): pytest.fail('failed persistence must not receive ACK')
    store = Store()
    collector = Collector(store, {'state_topic': 'state', 'event_topic': 'event'}, acknowledge=True)
    collector.client = Client()
    thread = threading.Thread(target=collector.run)
    thread.start()
    try:
        collector.queue.put(('event', b'{"v":1,"d":[1,1,1,1,1,7,1,12000]}', time.time(), False))
        deadline = time.monotonic() + 2
        while collector.write_errors == 0 and time.monotonic() < deadline: time.sleep(.01)
        assert collector.write_errors == 1 and collector.dropped == 0
        assert store.rejected == 0 and store.archive_error
        assert thread.is_alive()
    finally:
        collector.stop_event.set()
        thread.join(2)


def app_for_test(tmp_path):
    cfg = json.loads((Path(__file__).parents[1] / 'config.example.json').read_text(encoding='utf-8'))
    cfg['server'].update(database=str(tmp_path / 'health.db'), development_token='test-only')
    return create_app(cfg, collect=False)


def test_readiness_detects_dead_worker_subscription_and_storage(tmp_path, monkeypatch):
    app = app_for_test(tmp_path)
    collector, store = app.state.collector, app.state.store
    class Worker:
        def is_alive(self): return True
        def join(self, **kwargs): pass
    with TestClient(app) as client:
        assert client.get('/health').status_code == 200
        assert client.get('/ready').status_code == 503
        collector.thread = Worker()
        store.connection(True)
        assert client.get('/ready').status_code == 200
        store.connection(False)
        assert client.get('/ready').status_code == 503
        store.connection(True)
        store.archive_error = 'sensitive-internal-error'
        response = client.get('/ready')
        assert response.status_code == 503 and 'sensitive' not in response.text
        store.archive_error = None
        monkeypatch.setattr(store, 'database_ready', lambda: False)
        assert client.get('/ready').json()['database'] is False
        assert client.get('/ready').status_code == 503


def test_write_failure_counters_are_visible_after_recovery(tmp_path):
    app = app_for_test(tmp_path)
    app.state.collector.write_errors = 3
    app.state.collector.observer_errors = 2
    with TestClient(app) as client:
        token = client.post('/api/auth/local', json={'code': 'test-only'}).json()['token']
        response = client.get('/api/status', headers={'Authorization': 'Bearer ' + token})
        counters = response.json()['collector']
        assert counters['write_errors'] == 3 and counters['observer_errors'] == 2


@pytest.mark.parametrize('rollback_fails', [False, True])
def test_mysql_failed_commit_rolls_back_or_closes_before_reconnect(rollback_fails):
    calls = []
    class DB:
        def commit(self): calls.append('commit'); raise OSError('commit failed')
        def rollback(self):
            calls.append('rollback')
            if rollback_fails: raise OSError('connection lost')
        def close(self): calls.append('close')
    connection = Connection.__new__(Connection)
    connection.connection = DB()
    connection.in_transaction = True
    with pytest.raises(OSError, match='commit failed'):
        connection.__exit__(None, None, None)
    assert calls == (['commit', 'rollback', 'close'] if rollback_fails else ['commit', 'rollback'])
    assert not connection.in_transaction


def test_mysql_probe_uses_independent_bounded_connection(monkeypatch):
    class DB:
        closed = False
        def cursor(self): return self
        def __enter__(self): return self
        def __exit__(self, *args): pass
        def execute(self, sql): assert sql == 'SELECT 1'
        def fetchone(self): return {'1': 1}
        def close(self): self.closed = True
    probe = DB()
    def connect(**options):
        assert options['connect_timeout'] == options['read_timeout'] == options['write_timeout'] == 2
        return probe
    monkeypatch.setattr('backend.mysql_store.pymysql.connect', connect)
    connection = Connection.__new__(Connection)
    connection.options = {}
    connection.connection = object()  # The business connection must never be touched.
    assert connection.probe() and probe.closed
