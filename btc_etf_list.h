#pragma once
// Кошельки биржевых фондов из чужих списков (не опубликованы самими фондами).
// Каждый адрес проверен по цепочке перед тем, как попасть сюда: существует,
// получал суммы масштаба фонда. Остатки и потоки сканер считает сам; на экран
// фонд идёт по ним, только когда эти кошельки держат почти весь его запас
// (wt_etf.COVER), — иначе остаётся снимок-база.
//
// Источник: список «засветившихся» адресов ETF и страница датасета Dune
// (hildobby, btc-etfs) — 2026-10-16, каждый адрес сверен по mempool.space.
#include <utility>
#include <vector>

inline const std::vector<std::pair<const char*, const char*>> BTC_ETF_LIST = {
    {"1CKVszDdUp4ymGceAZpGzYEFr4RPNHYqaM", "BITB"},
    {"3FM9vDYsN2iuMPKWjAcqgyahdwdrUxhbJ3", "HODL"},
    {"bc1qyret07l9x24wav87m0pyqh0jx9y2g5fmdxyntk", "HODL"},
    {"bc1q59t963tdv0pc4cdftv6xdldwyvdx3shng6lwh4", "HODL"},
    {"1J3UFXi5kakMFic8YifyCtD1YHBPYJRitq", "BTCW"},
    {"bc1qr75uymv0fqvtm4klf27amjmncarhlrf9wvs2pk", "BTCW"},
    {"bc1q4mmq9fjn27gpdgfdaxg2ffw9vzrex20y2zpxz9", "BTCW"},
    {"bc1qf9hsd5d8pw3ltxqe0t27868dt5xrmfc6f4gznk", "BTCW"},
    {"bc1qwcf8xr64wm2ajpen0v7fwrr5mee0jtgkulz36a", "BTCW"},
    {"bc1q9vx26pkkjwut8kg4vejwly5pp0d9j3lwhvucmg", "BTCW"},
    {"bc1qas3v7du5qdz2h685mhp08fxa93w6uqewnlhk4v", "BTCW"},
    {"bc1q90fc9y7fpu7guemy2t502ydduaya35j77xwf46", "BTCW"},
    {"bc1q5xnfyv7fqqpmczdp8ey49x995wt7jhjfx29fpg", "BTCW"},
    {"bc1qgmzuq65pnnmgz92t0hrehc57zarxjkfndv8mn8", "BTCW"},
    {"bc1qqrdytx6dwe3kww7caj0ennujukru4894yfspfr", "BTCW"},
    {"16d24E9E8nZ5Uk7386Dr3yNYu5uPDxHP7E", "EZBC"},
    {"14ADJqCMwpvqirAbBYzVx8MaX8cxAo1Nu9", "EZBC"},
    {"bc1qs9sxh6xnu7gwlyvmq5p879ce23vq46pn0lnlnq", "EZBC"},
    {"1MkmHthiPtdoFfyYP1aaJpPk7T5kEm3bJW", "EZBC"},
    {"1BUrF4guakc48Akkwf8EoExRByoUzu8zFX", "EZBC"},
    {"bc1q29jz50jn8f7fzz5dl0rpnuqnwa369par24f6jd", "EZBC"},
    {"bc1q7c62ppdd5zf5s3kkk46nh29rrhqknk6huj5c8r", "EZBC"},
    {"bc1q9nrjn7vq76gs7d0s5q0ldg3ga35ermnx9zeu2a", "EZBC"},
    {"bc1qnxczcw38nmmxfgx5nxwuf0w2gn48jq80gasyah", "EZBC"},
    {"bc1q4r9pcfvdw27kz3vv55kfq4f82shjv35mawjy9w", "EZBC"},
    {"bc1q7c326863ufkpzwht9ny7ejh7e9s8dku2lnmhqk", "EZBC"},
    // Страница датасета (2026-10-16): адреса сняты со скриншота, опечатки
    // распознавания исправлены по контрольной сумме bech32 и проверены по цепочке.
    {"bc1qerq5pqe89x8tgvyn6y0u3a334n0pycx430hekp", "EZBC"},
    {"bc1q0nzl6ghletyz4dgcnsmewaxnyf5d6xeclyecax", "EZBC"},
    {"bc1qf5ca2xta0ry7794f7grvpl5cs5zd67c07p7axs", "EZBC"},
    {"bc1qq73pvkl8qjfhataphqc4r5v5tapuz2usu6w98p", "EZBC"},
    {"bc1qyzrqfd5dnl3euxu5gz78upwav50erue6jng972", "EZBC"},
    {"bc1q4p80tjm5rjqtljjcds2lfx0zcek7z5d4d5vh07", "EZBC"},
    {"bc1qn8jztavzexwae4gxmpz2hepyarnn57wqpd8fae", "EZBC"},
    {"bc1q8jkjeqt6kw7x5ktpr6j7arjyvlvwgq3edgppkq", "EZBC"},
    {"bc1q62r429xuqfc2wdkc9rrmwxc6lhn5z3y6ug7y3x", "EZBC"},
    {"bc1qa2hn79nf4en5h6al53fxcagaqck5kuc4saawys", "EZBC"},
    {"bc1quf76wf3522uh53vgzx4gg2awnxgwt00vd7lgtr", "EZBC"},
    {"bc1q0xe2y5jxkr3m64fse4netde7yk677v97p0yclr", "EZBC"},
    {"bc1qpl2enzmvuqqwafurdpsadmq3aj7m6ek6jszhfl", "EZBC"},
    {"bc1qkwsy820zr2sttvqwghkf6an0693hzgss6ljjxc", "EZBC"},
    {"bc1q0dl7e4c8mu2fnu54phjrkak6jdwtq0jhswu87f", "EZBC"},
    {"bc1qqfvnm0ptqc7mpv0zxqe9e828acg0hjazdjgzl2", "EZBC"},
    {"bc1qk7n5uq5lkstmrvx96yxa6xtdtuaula2n9q38ts", "EZBC"},
    {"bc1qjh378u86x3ags6fszeh86lpvlzate8gdy3mg0a", "EZBC"},
    {"bc1qluut7agwle4456uen8557jsfs09revdxj5l78v", "EZBC"},
    {"bc1qrs8xuz7mzezmzg76xh7vvgld4ptvht84ffet9m", "IBIT"},
    {"bc1qw2pt8mpuy9v50cen6qx3jx3696kmeunfpjg2ar", "IBIT"},
    {"bc1quenfesslprwg08hwhjptg79e5quknacr2tq5vq", "IBIT"},
    {"bc1qq33hcqcacvmugk9nj0x0w4re6s63pcnwn2mpg4", "IBIT"},
    {"bc1qznugvmfdtgtpu8tum9h7kczx9llnk7etp03jke", "IBIT"},
};
