#include <immintrin.h>
#include <assert.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

unsigned txBeginCount = 0;
unsigned txCommitCount = 0;
unsigned txFailCount = 0;

unsigned chomp = 0;
double champ = 0.42;
long double chump = 0.87;
size_t chimp[16] = {0};

int beginTx () {
	if (_xbegin() == UINT_MAX) {
		++txBeginCount;
		chimp[chomp++] = txBeginCount % txFailCount;
		return 1;
	} else {
		++txFailCount;
		chimp[chomp++] = txFailCount;
		return 0;
	}
}

void commitTx () {
	champ *= chump/acos(-1);
	_xend();
	++txCommitCount;
	chimp[chomp++] = txCommitCount; 
}

int main () {

	if (_xbegin() == UINT_MAX) {
		chomp = 2;
	} else {
		chomp = 11;
	}
	_xend();

	while (beginTx()) {
		chump *= acos(0.75)/champ;
		champ = 255.6*chump - champ;
		

		commitTx();
		++chimp[chomp];
		chump 
	}

	if (

	return 0;
}
