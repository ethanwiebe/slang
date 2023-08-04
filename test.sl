(print 'hi)
(+ 12 (+ 1 1))
(def abc 13)
(+ abc 1)
12
(def func 
	(& (y) 
				(def x 1)
				(+ x y)))



(def eq (& (x y)
				(if (- x y) false true)
				))

(def fact-sub (& (z curr)
	(if (eq z 1)
		curr 
		(fact-sub (-- z) (* curr z))
	)
))

(def fact (& (x) (fact-sub x 1)))

(def interm1 (& (x y) (+ x y)))
(def interm2 (& (x y) (* x y)))


(def combo (& (a b)
	(* (interm1 a b) (interm2 a b))
))

(print (combo 2 4))
(print (= 48 48))
(assert (= (combo 2 4) 48))
(print (combo (+ 2 4) (+ 4 6)))
(print (combo (combo 2 4) 1))
(assert (= (combo (combo 2 4) 17) 53040))

(def fact-linear (& (x)
	(print x)
	(if
		(= x 1)
		1
		(* x (fact-linear (- x 1)))
	)
))

(print combo)
(print (fact 50))
(print fact-linear)
(print 'hello)
(print (fact-linear 8))
;(print 'start)
;(print 'end)


(def nest 
	(& (x) 
		(& (y)
			(print x)
			(print y)
			(* x y)
		)
	)
)

(def counter
	(& (x)
		(let ((count x))
			(print count)
			(& () 
				(set! count (+ count 1))
			)
		)
	)
)

(def mul-by-3 (nest 3))
(print (mul-by-3 5))

(print 'c1)
(def c1 (counter 3))
(def c2 (counter 8))

(def test 
(& (x) (if (eq x 0) 0 (test (- x 1))))
)

(def a (/ 9 2))

(c1)
(c1)
(print (c1))
(print (c1))
(print (c2))
(assert (= (c1) 8))
(assert (= (c2) 10))

(print (= ''(1 (1 1.0)) ''(1 (1 1))))
(print (/ 2 -2))
(print (% 1 -10))
(assert (= '() ()))

(def x 10)
(print x)
(print (is '(1) ()))
(print (is (R '(1)) ()))

(def print-list (& (l)
	(print (L l))
	(if (null? (R l)) 
		0
		(print-list (R l))
	)
))

(print-list '(1 2 3 hello))

(def vary (& args
	(print args)
	(print-list args)
))

(vary 1 2 3 4 vary)

(print ((if true + *) 3 4))

(print ((& (x) (+ x 17)) 23))

(def lettest
	(let 
		((x 0)
		(y 1))
		(+ x y)
	)
)

(print lettest)

(def letcount
	(let
		((count 0))
		(& ()
			(set! count (++ count))
			count
		)
	)
)

(print (letcount))
(print (letcount))
(print (letcount))
(print (letcount))
(print (num? 3.2))
(print "tes\n\0\et")

(def v (vec 1 2 'hello 5))
(print (list 1 2 'hello 4))
(print v)
(print (vec-get v 3))
(print '-5)
(def aa -.3)
(print -aa)
(print .3)
(print (vec-set! v -1 'bye))
(def l (list 1 2 3))
(def va (vec-alloc 8 0))
(print va)
(vec-set! va 3 4)
(print va)
(print v)
(def var (gc-mem-size))
(print var)
(def var2 var)
(set! var2 10)
(print var)
(print var2)

(def limited-use (& (func max)
	(let ((count max))
		(& args (if !count 
			(assert false)
			(do
				(set! count (-- count))
				(apply func args)
			)
		))
		
	)
))

(def limit-add (limited-use + 3))
(print (limit-add 2 3))
(print (limit-add 2 3))
(print (limit-add 2 3))
