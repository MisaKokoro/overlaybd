#!/usr/bin/env python3
"""不启动服务的进程归属和 TCP 等待策略回归测试。"""
import importlib.util, pathlib, tempfile, unittest
from unittest.mock import patch
spec=importlib.util.spec_from_file_location('ctl',pathlib.Path(__file__).with_name('test.py'))
ctl=importlib.util.module_from_spec(spec);spec.loader.exec_module(ctl)
class Controls(unittest.TestCase):
    def test_reused_pid_is_not_ours(self):
        with patch.object(ctl,'record',return_value={'pid':42,'start':'100'}),patch.object(ctl,'proc_start',return_value='101'):
            self.assertFalse(ctl.alive('tcmu'))
    def test_dead_process_with_missing_start_is_not_alive(self):
        with patch.object(ctl,'record',return_value={'pid':42,'start':None}),patch.object(ctl,'proc_start',return_value=None):
            self.assertFalse(ctl.alive('docker'))
    def test_live_process_with_matching_start(self):
        with patch.object(ctl,'record',return_value={'pid':42,'start':'100'}),patch.object(ctl,'proc_start',return_value='100'):
            self.assertTrue(ctl.alive('docker'))
    def test_wait_for_ports_without_system_changes(self):
        import json
        with tempfile.TemporaryDirectory() as d:
            b=pathlib.Path(d);(b/'fixture').mkdir()
            (b/'fixture/manifest.json').write_text(json.dumps({'layers':[{'size':4096,'annotations':{'containerd.io/snapshot/overlaybd/zfile-config':'{"blockSize":4096}'}}]}))
            original=pathlib.Path.read_text
            def read(p,*a,**kw):
                if str(p)=='/proc/sys/net/ipv4/ip_local_port_range':return '32768 34767'
                return original(p,*a,**kw)
            rows='\n'.join('0 0 10.0.0.1:%d 10.0.0.2:9000'%(32768+i) for i in range(1000))
            with patch.object(ctl,'B',b),patch.object(pathlib.Path,'read_text',read),patch.object(ctl,'output',side_effect=[rows,'']),patch.object(ctl.time,'sleep') as sleep,patch.object(ctl,'run') as mutation:
                ctl.wait_tcp_budget({'cache_block_size':4096});sleep.assert_called_once_with(1);mutation.assert_not_called()
if __name__=='__main__':unittest.main()
