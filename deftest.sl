(def x 3)


(def foo 
	(& () 
		(do
			(def x 4)
			x
		)
	)
)

(def bar 
	(& ()
		(do
			(set! x 4)
			x
		)
	)
)

(print (foo))
(print x)
(print (bar))
(print x)
