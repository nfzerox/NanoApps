#ifndef TR_SCENE_TEST_H
#define TR_SCENE_TEST_H
/* Hardware test scene: the original track, trees, runner and monkeys, drawn the way the
 * game draws them, with the camera running down an endless straight. Not gameplay. */
int scene_init(void);
/* mode 0: base texture + lightmap (as the game); 1: base texture only; 2: track only */
void scene_frame(int w, int h, float dt, int mode);
extern int scene_overscan_x, scene_overscan_y;
extern int scene_no_trees;
extern int scene_indexed;        /* 1: draw the indexed meshes straight through GL (first test's path) */
extern int scene_stat_draws, scene_stat_vertices, scene_stat_triangles;
#endif
