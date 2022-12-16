(def x 3)


(def foo 
	(lambda () 
		(do
			(def x 4)
			x
		)
	)
)

(def bar 
	(lambda ()
		(do
			(set! x 4)
			x
		)
	)
)

(foo)
x
(bar)
x
