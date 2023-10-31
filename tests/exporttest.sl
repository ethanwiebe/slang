; export test

(def a 3)
(export a)

(def (f x) (* x x))
(export f)
(assert !(bound? 'b))

(def c 23)

(def (get-c) c)
(def (inc-c) (set! c (++ c)))
(export get-c)
(export inc-c)

