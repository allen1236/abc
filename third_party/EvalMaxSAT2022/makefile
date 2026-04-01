all:
	g++ ipamirEvalMaxSAT2022glue.cc -Ilib/cadical/src -Ilib/EvalMaxSAT/src/ -Ilib/MaLib/src/ lib/MaLib/src/*.h lib/cadical/src/*.cpp lib/EvalMaxSAT/src/* -lz -lpthread -c -Wall -DNDEBUG -O3 -std=c++17
	ar rvs libipamirEvalMaxSAT2022.a *.o
	rm *.o 2> /dev/null
	rm lib/EvalMaxSAT/src/*.gch 2> /dev/null
	rm lib/MaLib/src/*.gch 2> /dev/null

	### then compile with g++ main.cpp libipamirEvalMaxSAT.a -o main -lz 
