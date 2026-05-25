Could the bit be eliminated by structural means rather than encoded as state?

Yes: there are at least two cleaner alternatives:

An onAfterExtract() post-hook. Have the base LiveListener::extractData() template method call a new onAfterExtract() hook after doExtractData() returns normally (i.e., not on the NotYet path). The SNS override clears m_lastTransition there. No flag needed; the timing of the hook is the bit.
