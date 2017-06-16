//
//  ichabod.h
//  ichabod
//
//  Created by Charley Robinson on 6/15/17.
//

#ifndef ichabod_h
#define ichabod_h

/**
 * Entry point for building archives with this project.
 */

struct ichabod_s;

void ichabod_alloc(struct ichabod_s** ichabod_out);
void ichabod_free(struct ichabod_s* ichabod);
int ichabod_main(struct ichabod_s* ichabod);

#endif /* ichabod_h */
