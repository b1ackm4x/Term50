"""Apply the BB10 child-exit wakeup workaround to pristine Zsh 5.9."""
from pathlib import Path
import sys

source = Path(sys.argv[1]) / "Src/signals.c"
text = source.read_text()
marker = "/* TERM50_QNX_CHILD_WAIT_V2 */"
if marker not in text:
    include = '#include "zsh.mdh"'
    assert text.count(include) == 1
    text = text.replace(include, include + '\n#ifdef __QNXNTO__\n#include <sys/neutrino.h>\n#endif')
    original = '    ret = sigsuspend(&set);'
    assert text.count(original) == 1
    helper = """
#ifdef __QNXNTO__
static _Uint64t term50_child_wait_ns = 20000000;
/**/
void
term50_reset_child_wait(void)
{
    term50_child_wait_ns = 20000000;
}
#endif

"""
    text = text.replace('/* Install signal handler for given signal.', helper + '/* Install signal handler for given signal.')
    text = text.replace(original, '''# ifdef __QNXNTO__
    /* TERM50_QNX_CHILD_WAIT_V2 */
    /* BB10 can leave the shell waiting for a child that is already
     * waitable. Bound only this kernel wait, preserving normal signal
     * delivery and job control. Reap child status after a timeout.
     * This timer is cleared by the kernel when SignalSuspend returns. */
    {
	TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SIGSUSPEND,
	             NULL, &term50_child_wait_ns, NULL);
	ret = sigsuspend(&set);
	if (ret == -1 && errno == ETIMEDOUT) {
	    wait_for_processes();
	    if (term50_child_wait_ns < 250000000) {
	        term50_child_wait_ns *= 2;
	        if (term50_child_wait_ns > 250000000)
	            term50_child_wait_ns = 250000000;
	    }
	}
    }
# else
    ret = sigsuspend(&set);
# endif''')
    source.write_text(text)

    jobs = source.parent / "jobs.c"
    job_text = jobs.read_text()
    for anchor in ["    int first = 1, q = queue_signal_level();", "    Job jn = jobtab + job;"]:
        assert job_text.count(anchor) == 1
        job_text = job_text.replace(anchor, anchor + "\n#ifdef __QNXNTO__\n    term50_reset_child_wait();\n#endif")
    jobs.write_text(job_text)
