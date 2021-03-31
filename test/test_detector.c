#include <immintrin.h>
#include <assert.h>
#include <stdio.h>
#include <limits.h>

unsigned txBeginCount = 0;
unsigned txCommitCount = 0;
unsigned txFailCount = 0;

int beginTx () {
	if (_xbegin() == UINT_MAX) {
		++txBeginCount;
		return 1;
	} else {
		++txFailCount;
		return 0;
	}
}

void commitTx () {
	_xend();
	++txCommitCount;
}

unsigned chomp;
int main () {

	if (_xbegin() == UINT_MAX) {
		chomp = 2;
	} else {
		chomp = 11;
	}
	_xend();

	if (beginTx()) {
		++chomp;
	} else {
		--chomp;
	}
	commitTx();

	return 0;
}
