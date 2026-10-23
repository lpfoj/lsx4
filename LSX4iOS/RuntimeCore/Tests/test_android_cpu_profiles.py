import importlib.util,json,tempfile,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('profiles',Path(__file__).resolve().parents[1]/'android_cpu_profiles.py')
profiles=importlib.util.module_from_spec(spec);spec.loader.exec_module(profiles)

class CpuProfileHandoff(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.path=self.root/'CUSA11628.ini'
        self.path.write_text('# title_id=CUSA11628\nmax_guest_workers=32\n')
        self.manifest={'version':1,'resolverSha256':profiles.sha(profiles.SOURCE),
            'profiles':{'CUSA11628':profiles.sha(self.path)}}
        self.publish()
    def publish(self):
        (self.root/'manifest.json').write_text(json.dumps(self.manifest))
    def test_actual_source_fingerprint_and_export_are_both_required(self):
        self.assertEqual(profiles.select_profile(self.root,'CUSA11628'),self.path)
        self.path.write_text(self.path.read_text()+'block_linking=0\n')
        with self.assertRaisesRegex(ValueError,'checksum'):profiles.select_profile(self.root,'CUSA11628')
    def test_source_update_requires_regeneration(self):
        self.manifest['resolverSha256']='outdated';self.publish()
        with self.assertRaisesRegex(ValueError,'stale'):profiles.select_profile(self.root,'CUSA11628')
    def test_mismatched_title_never_stages_a_profile(self):
        with self.assertRaisesRegex(ValueError,'another title'):
            profiles.stage_profile(self.path,self.root/'run','CUSA00920')
    def test_default_launch_cannot_reuse_previous_games_profile(self):
        first=profiles.stage_profile(self.path,self.root/'run','CUSA11628')
        second=profiles.stage_profile(None,self.root/'run','CUSA00920')
        self.assertEqual(first,second)
        self.assertNotIn('max_guest_workers=32',second.read_text())
        self.assertEqual(json.loads((self.root/'run/cpu-profile.json').read_text())['source'],'native-default')

    def route(self, requested='auto', **changes):
        fields=dict(execution_mode=0,fast_memory=1,memory_manager=1,ignore_bad_accesses=0)
        fields.update(changes)
        self.path.write_text('# title_id=CUSA11628\n'+''.join(f'{k}={v}\n' for k,v in fields.items()))
        before=self.path.read_bytes()
        result=profiles.resolve_backend(requested,self.path)
        self.assertEqual(self.path.read_bytes(),before)
        self.assertEqual(result['profileSha256'],profiles.sha(self.path))
        return result

    def test_fex_compatible_profile_and_both_android_ir_policies(self):
        self.assertEqual(self.route()['effective'],'fex')
        self.assertEqual(self.route(execution_mode=1)['effective'],'current')
        thomas=self.route(execution_mode=1,fast_memory=2,memory_manager=3,ignore_bad_accesses=1)
        self.assertEqual(thomas['effective'],'current')
        self.assertEqual(thomas['currentPolicyFields'],['execution_mode','ignore_bad_accesses'])

    def test_current_only_modes_and_memory_semantics_do_not_enter_fex(self):
        for policy in (dict(execution_mode=2),dict(execution_mode=3),dict(fast_memory=0),
                       dict(memory_manager=0),dict(memory_manager=2),dict(ignore_bad_accesses=1)):
            with self.subTest(policy=policy):self.assertEqual(self.route(**policy)['effective'],'current')
        self.assertEqual(self.route(memory_manager=3,fast_memory=2)['effective'],'fex')

    def test_explicit_engine_does_not_silently_fallback(self):
        self.assertEqual(self.route('fex',execution_mode=1,ignore_bad_accesses=1)['effective'],'fex')
        self.assertEqual(self.route('current')['effective'],'current')

    def test_missing_malformed_duplicate_and_unknown_policy_fail_closed(self):
        for text in ('', 'execution_mode=1\n',
                     'execution_mode=-1\n', 'execution_mode=4\n',
                     'execution_mode=one\n','execution_mode=1\nexecution_mode=0\n'):
            self.path.write_text(text)
            with self.subTest(text=text),self.assertRaises(ValueError):
                profiles.resolve_backend('auto',self.path)
        with self.assertRaises(ValueError):self.route('unknown')

if __name__=='__main__':unittest.main()
